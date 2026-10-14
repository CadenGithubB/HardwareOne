#!/usr/bin/env python3
"""Stage the pinned ESP-Hosted 2.12.13 DMA-stride fix for P4; leave the package unchanged.

Wi-Fi's common TX pool must align every block, not only the allocation base.
The tracked patch derives its stride from ESP-IDF 5.5.5's allocator hook.
Source, patch and prepared tree hashes are checked; an existing destination
is verified, never overwritten. See esp-hosted-2.12.13-hw1.md.
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
NAME = "espressif__esp_hosted"
VERSION = "2.12.13"
COMPONENT_HASH = "f1792f36ef9a637adf2faa2662f3c4500b07f3d2ba65c25f3ed4824490ba59f9"
SOURCE_TREE_SHA256 = "708e07b09d8c49cfb4c52068ba20b461f8885f2eaa8e47753fd82f82c8e64824"
PATCH = ROOT / "esp-hosted-2.12.13-hw1.patch"
PATCH_SHA256 = "ed7293f2038d8e0e792288ecd18932f9432de8bb507061377e5d345a7b063ee1"
PATCHED_TREE_SHA256 = "d933321e456f0cc9609875beaa0436df7e12ad0963d8af804514f931d2206208"
MANIFEST = "hw1-esp-hosted-override.json"
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
        f"no pristine esp-hosted {VERSION} (component hash {COMPONENT_HASH[:8]}) found in "
        f"{'--source' if source else 'managed_components/ or the component manager cache'}. "
        "Pass --source DIR with the espressif__esp_hosted directory a P4 build of an "
        "earlier checkout left in managed_components/, or the package from "
        f"components.espressif.com (espressif/esp_hosted {VERSION}).")


def expected_manifest() -> dict:
    return {"schema": 1, "component": "espressif/esp_hosted", "version": VERSION,
            "component_hash": COMPONENT_HASH, "source_tree_sha256": SOURCE_TREE_SHA256,
            "patch_sha256": PATCH_SHA256, "tree_sha256": PATCHED_TREE_SHA256}


def verify(destination: Path) -> None:
    if destination.is_symlink():
        raise ValueError("override destination must not be a symlink")
    manifest = json.loads((destination / MANIFEST).read_text())
    if manifest != expected_manifest():
        raise ValueError("ESP-Hosted override provenance mismatch (re-stage it: remove the directory and rerun)")
    if tree_digest(destination) != PATCHED_TREE_SHA256:
        raise ValueError("ESP-Hosted override changed after preparation")


def prepare(destination: Path, source: Path | None, check: bool = False) -> Path:
    if digest(PATCH.read_bytes()) != PATCH_SHA256:
        raise ValueError("ESP-Hosted patch differs from the reviewed version")
    if destination.exists() or destination.is_symlink():
        verify(destination)
        return destination
    if check:
        raise ValueError("ESP-Hosted override has not been prepared")
    pristine = find_source(source)
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".esp-hosted-", dir=destination.parent))
    try:
        shutil.copytree(pristine, stage, dirs_exist_ok=True, ignore=shutil.ignore_patterns(*IGNORED))
        applied = subprocess.run(["patch", "-f", "-F", "0", "-p", "1"], input=PATCH.read_bytes(),
                                 cwd=stage, capture_output=True)
        if applied.returncode:
            raise ValueError("ESP-Hosted patch failed: " + applied.stdout.decode() + applied.stderr.decode())
        for leftover in list(stage.rglob("*.orig")) + list(stage.rglob("*.rej")):
            leftover.unlink()
        if tree_digest(stage) != PATCHED_TREE_SHA256:
            raise ValueError("prepared ESP-Hosted override does not match the reviewed result")
        (stage / MANIFEST).write_text(json.dumps(expected_manifest(), indent=2, sort_keys=True) + "\n")
        stage.rename(destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    verify(destination)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, help="a pristine espressif__esp_hosted 2.12.13 directory")
    parser.add_argument("--destination", type=Path, default=REPO / ".sdk-overrides/esp32p4" / NAME)
    parser.add_argument("--check", action="store_true", help="verify an existing override; stage nothing")
    args = parser.parse_args()
    try:
        output = prepare(args.destination.absolute(), args.source, args.check)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"ESP-Hosted override: {exc}\n")
    print(f"Verified ESP-Hosted override: {output}")


if __name__ == "__main__":
    sys.exit(main())
