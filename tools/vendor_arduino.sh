#!/usr/bin/env bash
# vendor_arduino.sh — make a fresh clone buildable: vendor arduino-esp32 3.3.5 into
# components/arduino (git-ignored) and apply HardwareOne's local patches in order.
#
#   tools/vendor_arduino.sh            # vendor + patch + verify (refuses to touch an existing copy)
#   tools/vendor_arduino.sh --check    # only run the patch verifier on the existing copy
#   tools/vendor_arduino.sh --force    # move an existing components/arduino aside and re-vendor
#   tools/vendor_arduino.sh --into DIR # vendor into DIR instead (for reproducing/testing the sequence)
#
# The result is a nested git checkout of upstream commit $UPSTREAM_SHA (tag 3.3.5) with the
# patches applied as working-tree changes, exactly the layout docs/arduino-local-patches/README.md
# describes, so `git -C components/arduino diff` keeps working for the patch-refresh step.
# tools/build_board.sh and tools/build_deployment.sh run verify_patches.sh before every build.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
UPSTREAM_URL="https://github.com/espressif/arduino-esp32.git"
UPSTREAM_SHA="11bc7ac1a458f1f4e7afc8fd4de1dfa710f31562"   # tag 3.3.5
PATCH_DIR="$REPO/docs/arduino-local-patches"
PATCHES=(
  "$PATCH_DIR/arduino-local-patches.patch"
  "$PATCH_DIR/arduino-local-patches-2026-08-30-url-ota-removal.patch"
  "$PATCH_DIR/arduino-local-patches-2026-10-02-companion.patch"
  "$PATCH_DIR/arduino-local-patches-2026-10-15-panic-iram.patch"
  "$PATCH_DIR/arduino-local-patches-2026-10-22-ble-memory.patch"
  "$PATCH_DIR/arduino-local-patches-2026-10-25-ble-search-wait.patch"
  "$PATCH_DIR/arduino-local-patches-2026-11-06-ble-connect-semaphores.patch"
  "$REPO/tools/ble-phone-pipe/arduino.patch"
)

DEST="$REPO/components/arduino"
MODE="vendor"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --check) MODE="check" ;;
    --force) MODE="force" ;;
    --into) shift; DEST="${1:?--into needs a directory}" ;;
    -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
    *) echo "error: unknown argument $1" >&2; exit 2 ;;
  esac
  shift
done

verify() {
  # verify_patches.sh checks $REPO/components/arduino; for --into it is pointed at DEST by a temporary link.
  if [[ "$DEST" == "$REPO/components/arduino" ]]; then
    bash "$PATCH_DIR/verify_patches.sh"
  else
    local tmp
    tmp="$(mktemp -d)"
    mkdir -p "$tmp/components" "$tmp/docs" "$tmp/tools"
    ln -s "$DEST" "$tmp/components/arduino"
    ln -s "$PATCH_DIR" "$tmp/docs/arduino-local-patches"
    ( cd "$tmp" && bash docs/arduino-local-patches/verify_patches.sh )
    local rc=$?
    rm -rf "$tmp"
    return $rc
  fi
}

if [[ "$MODE" == "check" ]]; then
  [[ -d "$DEST" ]] || { echo "error: $DEST does not exist; run $0 to vendor it" >&2; exit 1; }
  verify && echo "components/arduino: vendored and patched (OK)"
  exit $?
fi

for p in "${PATCHES[@]}"; do
  [[ -f "$p" ]] || { echo "error: missing patch $p" >&2; exit 1; }
done
command -v git >/dev/null || { echo "error: git is required" >&2; exit 1; }

if [[ -e "$DEST" ]]; then
  if [[ "$MODE" == "force" ]]; then
    aside="$DEST.previous-$(date +%Y%m%d-%H%M%S)"
    mv "$DEST" "$aside"
    echo "moved the existing copy to $aside"
  else
    echo "error: $DEST already exists." >&2
    echo "       Run with --check to verify it, or --force to move it aside and re-vendor." >&2
    exit 1
  fi
fi

echo "cloning arduino-esp32 $UPSTREAM_SHA (tag 3.3.5) into $DEST"
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin "$UPSTREAM_URL"
if ! git -C "$DEST" fetch -q --depth 1 origin "$UPSTREAM_SHA"; then
  echo "shallow fetch by commit refused; fetching tag 3.3.5" >&2
  git -C "$DEST" fetch -q --depth 1 origin refs/tags/3.3.5:refs/tags/3.3.5
fi
git -C "$DEST" checkout -q "$UPSTREAM_SHA"
[[ "$(git -C "$DEST" rev-parse HEAD)" == "$UPSTREAM_SHA" ]] || { echo "error: checkout is not $UPSTREAM_SHA" >&2; exit 1; }

for p in "${PATCHES[@]}"; do
  echo "applying $(basename "$p")"
  git -C "$DEST" apply --check "$p"
  git -C "$DEST" apply "$p"
done

verify
echo "components/arduino: vendored at $UPSTREAM_SHA and patched (OK). Next: tools/build_board.sh <board>"
