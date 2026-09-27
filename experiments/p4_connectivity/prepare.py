#!/usr/bin/env python3
"""Reproduce this experiment from the verified p4_mesh app snapshot.

Only --refresh writes the durable manifest/patches. Normal preparation creates a
new destination atomically and never changes the baseline or an existing app.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
PATCHES = ("app-overlay.patch", "arduino-overlay.patch")
NEW_SOURCES = (
    "components/hardwareone/HAL_Bluetooth.h",
    "components/hardwareone/HAL_Bluetooth.cpp",
    "components/hardwareone/Connectivity_BleProbe.h",
    "components/hardwareone/Connectivity_BleProbe.cpp",
    "components/hardwareone/Connectivity_HttpProbe.h",
    "components/hardwareone/Connectivity_HttpProbe.cpp",
)
BUILD_INPUTS = tuple("p4_connectivity/" + name for name in (
    "features.h", "sdkconfig.connectivity.defaults", "sdkconfig.p4.bluetooth.defaults",
    "radio_backend.cpp", "radio_backend.h", "build-p4.sh", "build-s3.sh",
)) + tuple("p4_espnow/bridge/" + name for name in (
    "esp_now_hosted_host.c", "esp_now_hosted_host.h", "esp_now_hosted_rpc.h",
))


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_path(root: Path, relative: str) -> Path:
    rel = PurePosixPath(relative)
    if rel.is_absolute() or not rel.parts or ".." in rel.parts or "\\" in relative:
        raise ValueError(f"unsafe source path: {relative!r}")
    path = root.joinpath(*rel.parts)
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError(f"source path escapes tree: {relative}")
    if path.is_symlink():
        raise ValueError(f"source symlink is unsupported: {relative}")
    return path


def verify_files(tree: Path, expected: dict[str, str], label: str) -> None:
    failures = []
    for rel, sha in expected.items():
        path = safe_path(tree, rel)
        if not path.is_file():
            failures.append(f"missing {rel}")
        elif digest(path.read_bytes()) != sha:
            failures.append(f"changed {rel}")
    if failures:
        raise ValueError(f"{label} mismatch ({len(failures)} files): " + "; ".join(failures[:12]))


def load_manifest() -> dict:
    manifest = json.loads((ROOT / "source-manifest.json").read_text())
    if manifest.get("schema") != 1:
        raise ValueError("unsupported source manifest schema")
    for filename in PATCHES:
        if digest((ROOT / filename).read_bytes()) != manifest["patches"][filename]:
            raise ValueError(f"patch hash mismatch: {filename}")
    verify_files(ROOT.parent, manifest.get("build_inputs", {}), "durable build inputs")
    return manifest


def output_files(manifest: dict) -> dict[str, str]:
    expected = dict(manifest["baseline"])
    for rel, sha in manifest["changes"].items():
        if sha is None:
            expected.pop(rel, None)
        else:
            expected[rel] = sha
    return expected


def check_destination(destination: Path, manifest: dict) -> int:
    expected = output_files(manifest)
    verify_files(destination, expected, "prepared source")
    for rel, sha in manifest["changes"].items():
        if sha is None and safe_path(destination, rel).exists():
            raise ValueError(f"deleted source still present: {rel}")
    # Build/managed-component caches and other unrecorded files are deliberately
    # outside this source check; they are never copied by prepare().
    return len(expected)


def prepare(baseline: Path, destination: Path, manifest: dict) -> int:
    if destination.exists() or destination.is_symlink():
        raise ValueError(f"destination already exists; use --check: {destination}")
    verify_files(baseline, manifest["baseline"], "p4_mesh baseline")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=f".{destination.name}-prepare-", dir=destination.parent))
    try:
        for rel in manifest["baseline"]:
            target = safe_path(stage, rel)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(safe_path(baseline, rel), target)
        for filename in PATCHES:
            result = subprocess.run(
                ["patch", "-f", "-F", "0", "-p", "1", "-i", str(ROOT / filename)],
                cwd=stage, text=True, capture_output=True,
            )
            if result.returncode:
                raise ValueError(f"cannot apply {filename}:\n{result.stdout}{result.stderr}")
        # BSD patch may leave an empty file for a /dev/null deletion.
        for rel, sha in manifest["changes"].items():
            path = safe_path(stage, rel)
            if sha is None and path.is_file() and path.stat().st_size == 0:
                path.unlink()
        count = check_destination(stage, manifest)
        if destination.exists() or destination.is_symlink():
            raise ValueError(f"destination appeared during preparation: {destination}")
        stage.rename(destination)
        return count
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def unified_patch(before: bytes | None, after: bytes | None, rel: str) -> str:
    old = [] if before is None else before.decode("utf-8").splitlines(keepends=True)
    new = [] if after is None else after.decode("utf-8").splitlines(keepends=True)
    lines = difflib.unified_diff(
        old, new, fromfile="/dev/null" if before is None else "a/" + rel,
        tofile="/dev/null" if after is None else "b/" + rel,
    )
    return "".join(line if line.endswith("\n") else line + "\n\\ No newline at end of file\n" for line in lines)


def refresh(baseline: Path, destination: Path) -> dict:
    if not destination.is_dir():
        raise ValueError(f"private source directory is missing: {destination}")
    manifest_path = ROOT / "source-manifest.json"
    if manifest_path.exists():
        old_manifest = json.loads(manifest_path.read_text())
        before_hashes = old_manifest["baseline"]
        prior_changes = old_manifest["changes"]
    else:
        before_hashes = json.loads((ROOT / "private/baseline.json").read_text())
        prior_changes = {}
    verify_files(baseline, before_hashes, "p4_mesh baseline")
    changes = {}
    patches = {filename: [] for filename in PATCHES}
    for rel in sorted(set(before_hashes) | set(prior_changes) | set(NEW_SOURCES)):
        before = safe_path(baseline, rel).read_bytes() if rel in before_hashes else None
        path = safe_path(destination, rel)
        after = path.read_bytes() if path.is_file() else None
        if before == after:
            continue
        changes[rel] = digest(after) if after is not None else None
        filename = PATCHES[1] if rel.startswith("components/arduino/") else PATCHES[0]
        patches[filename].append(unified_patch(before, after, rel))
    patch_data = {filename: "".join(parts).encode("utf-8") for filename, parts in patches.items()}
    manifest = {
        "schema": 1,
        "baseline_description": "Verified p4_mesh/private/app source snapshot; includes its patched Arduino 3.3.5",
        "baseline": dict(sorted(before_hashes.items())),
        "changes": changes,
        "patches": {filename: digest(data) for filename, data in patch_data.items()},
        "build_inputs": {rel: digest(safe_path(ROOT.parent, rel).read_bytes()) for rel in BUILD_INPUTS},
    }
    for filename, data in patch_data.items():
        (ROOT / filename).write_bytes(data)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="check recorded output sources without modifying the app")
    mode.add_argument("--refresh", action="store_true", help="maintainer: recapture patches from the frozen private app")
    parser.add_argument("--baseline", type=Path, default=ROOT.parent / "p4_mesh/private/app")
    parser.add_argument("--destination", type=Path, default=ROOT / "private/app")
    args = parser.parse_args()
    try:
        if args.refresh:
            manifest = refresh(args.baseline, args.destination)
            print(f"Captured {len(manifest['changes'])} changed/new source files; baseline unchanged.")
        else:
            manifest = load_manifest()
            count = check_destination(args.destination, manifest) if args.check else prepare(args.baseline, args.destination, manifest)
            print(f"Verified {count} prepared source files: {args.destination}")
    except (OSError, ValueError, KeyError, UnicodeError) as error:
        parser.exit(1, f"prepare: {error}\n")


if __name__ == "__main__":
    main()
