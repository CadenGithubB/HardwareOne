#!/usr/bin/env python3
"""Stage the HardwareOne ESP-DL 3.3.12 override for ESP32-P4 builds; never edit the original.

Every ESP32-P4 build replaces the managed espressif/esp-dl with this copy (the
top-level CMakeLists.txt adds .sdk-overrides/esp32p4/espressif__esp-dl to
EXTRA_COMPONENT_DIRS). It is the pristine 3.3.12 package that
dependencies.lock.esp32p4 pins plus tools/p4/esp-dl-3.3.12-hw1.patch:

  - pointwise-conv tiles sized for the P4's shared 64 KB L1D (16 KB filter
    tile instead of 32 KB, 64-pixel spatial tile instead of 32; bit-exact,
    only the order of tile calls changes);
  - single-column int8 depthwise layers ([K,1] filters) run per 64-channel
    block so the sliding window and the block's filter stay in L1, and split
    across the two cores by channel halves at every length instead of by
    rows (bit-identical output: channels are independent);
  - prepared conv args: while hw1_dl_prepare_mode is set (dl_module_base.cpp)
    a pass over the model builds every Conv layer's ConvOpArgs and keeps a
    copy in a per-layer metadata block, and Add, LUT and Relu return; each
    Conv then runs on its prepared args once (same args, bit-identical
    output). With the flag never set every module runs as upstream;
  - dl_tool_metadata.{hpp,cpp}: every block a dl::Model keeps for its lifetime
    is requested through one allocator hook. With no hook installed each site
    behaves exactly as upstream; the STT runtime installs an explicit PSRAM
    request (components/hardwareone/stt/stt_dl_metadata.cpp, ps_alloc);
  - dl_fft pinned to ==0.7.0: a local component's dependencies join the root
    solve, which would otherwise be free to pick a newer dl_fft.

The pristine package comes from --source, this checkout's managed_components/
or the IDF component manager cache, and must match the pinned tree digest
before the patch is applied. An existing destination is verified, never
overwritten.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
NAME = "espressif__esp-dl"
VERSION = "3.3.12"
COMPONENT_HASH = "d792e72ab3858d805d8918d1215a862541344625f62134597866834de666fd04"
SOURCE_TREE_SHA256 = "e5c5155078c46e090ad7f08a7239bb52d456394fab1356d78b4c127eefcb6e86"
PATCH = ROOT / "esp-dl-3.3.12-hw1.patch"
PATCH_SHA256 = "e4b93864e4bfbb3154d166e7e5f486e8e9ae67f4ae111448764da30ed55c88c0"
PATCHED_TREE_SHA256 = "9355db26d0f2b725006ed6584e02fb4eaf29fa60a7478a926e587bf44c377d5f"
MANIFEST = "hw1-esp-dl-override.json"
# Present only in component-manager copies; not part of the package itself.
IGNORED = {".component_hash", MANIFEST}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tree_digest(root: Path) -> str:
    entries = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"unexpected symlink: {path}")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            if relative not in IGNORED:
                entries[relative] = digest(path.read_bytes())
    sha = hashlib.sha256()
    for relative, value in sorted(entries.items()):
        sha.update(relative.encode() + b"\0" + bytes.fromhex(value))
    return sha.hexdigest()


def cache_roots() -> list[Path]:
    roots = []
    if os.environ.get("IDF_COMPONENT_CACHE_PATH"):
        roots.append(Path(os.environ["IDF_COMPONENT_CACHE_PATH"]))
    try:  # the component manager's own answer, when its Python package is importable
        from idf_component_tools.file_cache import FileCache  # type: ignore
        roots.append(Path(FileCache().path()))
    except Exception:
        pass
    home = Path.home()
    roots += [home / "Library/Caches/Espressif/ComponentManager", home / ".cache/Espressif/ComponentManager"]
    if os.environ.get("LOCALAPPDATA"):
        roots.append(Path(os.environ["LOCALAPPDATA"]) / "Espressif/ComponentManager/Cache")
    return roots


def candidates(source: Path | None) -> list[Path]:
    if source is not None:
        return [source]
    found = [REPO / "managed_components" / NAME]
    for root in cache_roots():
        if root.is_dir():
            found += sorted(root.glob(f"*/{NAME}_{VERSION}_*"))
    return found


def find_source(source: Path | None) -> Path:
    for path in candidates(source):
        if (path / "idf_component.yml").is_file() and tree_digest(path) == SOURCE_TREE_SHA256:
            return path
    raise ValueError(
        f"no pristine esp-dl {VERSION} (component hash {COMPONENT_HASH[:8]}) found in "
        f"{'--source' if source else 'managed_components/ or the component manager cache'}. "
        "Pass --source DIR with the espressif__esp-dl directory a P4 build of an "
        "earlier checkout left in managed_components/, or the package from "
        f"components.espressif.com (espressif/esp-dl {VERSION}).")


def expected_manifest() -> dict:
    return {"schema": 1, "component": "espressif/esp-dl", "version": VERSION,
            "component_hash": COMPONENT_HASH, "source_tree_sha256": SOURCE_TREE_SHA256,
            "patch_sha256": PATCH_SHA256, "tree_sha256": PATCHED_TREE_SHA256}


def verify(destination: Path) -> None:
    if destination.is_symlink():
        raise ValueError("override destination must not be a symlink")
    manifest = json.loads((destination / MANIFEST).read_text())
    if manifest != expected_manifest():
        raise ValueError("ESP-DL override provenance mismatch (re-stage it: remove the directory and rerun)")
    if tree_digest(destination) != PATCHED_TREE_SHA256:
        raise ValueError("ESP-DL override changed after preparation")


def prepare(destination: Path, source: Path | None, check: bool = False) -> Path:
    if digest(PATCH.read_bytes()) != PATCH_SHA256:
        raise ValueError("ESP-DL patch differs from the reviewed version")
    if destination.exists() or destination.is_symlink():
        verify(destination)
        return destination
    if check:
        raise ValueError("ESP-DL override has not been prepared")
    pristine = find_source(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".esp-dl-", dir=destination.parent))
    try:
        shutil.copytree(pristine, stage, dirs_exist_ok=True, ignore=shutil.ignore_patterns(*IGNORED))
        applied = subprocess.run(["patch", "-f", "-F", "0", "-p", "1"], input=PATCH.read_bytes(),
                                 cwd=stage, capture_output=True)
        if applied.returncode:
            raise ValueError("ESP-DL patch failed: " + applied.stdout.decode() + applied.stderr.decode())
        for leftover in list(stage.rglob("*.orig")) + list(stage.rglob("*.rej")):
            leftover.unlink()
        if tree_digest(stage) != PATCHED_TREE_SHA256:
            raise ValueError("prepared ESP-DL override does not match the reviewed result")
        (stage / MANIFEST).write_text(json.dumps(expected_manifest(), indent=2, sort_keys=True) + "\n")
        stage.rename(destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    verify(destination)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, help="a pristine espressif__esp-dl 3.3.12 directory")
    parser.add_argument("--destination", type=Path, default=REPO / ".sdk-overrides/esp32p4" / NAME)
    parser.add_argument("--check", action="store_true", help="verify an existing override; stage nothing")
    args = parser.parse_args()
    try:
        output = prepare(args.destination.absolute(), args.source, args.check)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"ESP-DL override: {exc}\n")
    print(f"Verified ESP-DL override: {output}")


if __name__ == "__main__":
    sys.exit(main())
