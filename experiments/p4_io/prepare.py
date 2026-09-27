#!/usr/bin/env python3
"""Reconstruct LCD/input support over the qualified BLE-role source snapshot.

Default preparation creates a new destination atomically. --check is read-only.
--refresh captures an intentionally frozen private merge; it never merges or
edits application sources. Previous experiment snapshots remain unchanged.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent
PREVIOUS = ROOT.parent / "p4_ble_roles"
PERIPHERAL_PROVENANCE = ROOT / "provenance/peripheral-source-manifest.json"
spec = importlib.util.spec_from_file_location("io_roles_prepare", PREVIOUS / "prepare.py")
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)
NEW_SOURCES = tuple("components/hardwareone/" + name for name in (
    "System_Board_P4X_EYE.h", "HAL_SDCard.h", "HAL_SDCard.cpp",
    "HAL_Display_P4Eye.h", "HAL_Display_P4Eye.cpp", "HAL_Display_P4EyePixels.h",
    "Input_GPIOEncoder.h", "Input_GPIOEncoder.cpp", "Input_RotaryCore.h", "Input_ButtonCore.h",
))
BUILD_INPUTS = tuple("p4_io/" + name for name in (
    "prepare.py", "features.h", "build-p4.sh", "radio_backend.cpp", "radio_backend.h",
    "provenance/peripheral-source-manifest.json",
)) + tuple("p4_ble_roles/" + name for name in (
    "prepare.py", "features.h", "radio_backend.cpp", "radio_backend.h",
    "sdkconfig.connectivity.defaults", "sdkconfig.p4.bluetooth.defaults",
    "prepare_idf_bt.py", "idf-bt-manifest.json", "idf-tinycrypt-9fd7cb7.patch",
))


def baseline_files():
    return shared.output_files(shared.load_manifest())


def load_manifest():
    manifest = json.loads((ROOT / "source-manifest.json").read_text())
    if manifest.get("schema") != 1:
        raise ValueError("unsupported P4 I/O manifest schema")
    if manifest["baseline_manifest_sha256"] != shared.digest((PREVIOUS / "source-manifest.json").read_bytes()):
        raise ValueError("BLE-role baseline manifest changed")
    if manifest["peripheral_manifest_sha256"] != shared.digest(PERIPHERAL_PROVENANCE.read_bytes()):
        raise ValueError("peripheral provenance manifest changed")
    if manifest["patch_sha256"] != shared.digest((ROOT / "app-overlay.patch").read_bytes()):
        raise ValueError("P4 I/O overlay changed")
    shared.verify_files(ROOT.parent, manifest["build_inputs"], "P4 I/O build inputs")
    return manifest


def expected_files(manifest):
    expected = baseline_files()
    for relative, sha in manifest["changes"].items():
        if sha is None:
            expected.pop(relative, None)
        else:
            expected[relative] = sha
    return expected


def check(destination, manifest):
    shared.verify_files(destination, expected_files(manifest), "P4 I/O source")
    for relative, sha in manifest["changes"].items():
        if sha is None and shared.safe_path(destination, relative).exists():
            raise ValueError(f"deleted source remains: {relative}")
    return len(expected_files(manifest))


def prepare(baseline, destination, manifest):
    if destination.exists() or destination.is_symlink():
        raise ValueError(f"destination already exists; use --check: {destination}")
    before = baseline_files()
    shared.verify_files(baseline, before, "BLE-role baseline")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".p4-io-prepare-", dir=destination.parent))
    try:
        for relative in before:
            target = shared.safe_path(stage, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(shared.safe_path(baseline, relative), target)
        result = subprocess.run(
            ["patch", "-f", "-F", "0", "-p", "1", "-i", str(ROOT / "app-overlay.patch")],
            cwd=stage, capture_output=True, text=True, timeout=60,
        )
        if result.returncode:
            raise ValueError(f"P4 I/O patch failed:\n{result.stdout}{result.stderr}")
        # BSD patch may retain an empty file for a /dev/null deletion.
        for relative, sha in manifest["changes"].items():
            target = shared.safe_path(stage, relative)
            if sha is None and target.is_file() and target.stat().st_size == 0:
                target.unlink()
        count = check(stage, manifest)
        if destination.exists() or destination.is_symlink():
            raise ValueError("destination appeared during preparation; refusing replacement")
        stage.rename(destination)
        return count
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def refresh(baseline, destination):
    if not destination.is_dir() or destination.is_symlink():
        raise ValueError("a real, completed private source copy is required for --refresh")
    before = baseline_files()
    shared.verify_files(baseline, before, "BLE-role baseline")
    changes, patch = {}, []
    for relative in sorted(set(before) | set(NEW_SOURCES)):
        original = shared.safe_path(baseline, relative)
        target = shared.safe_path(destination, relative)
        old = original.read_bytes() if relative in before else None
        new = target.read_bytes() if target.is_file() else None
        if old != new:
            changes[relative] = shared.digest(new) if new is not None else None
            patch.append(shared.unified_patch(old, new, relative))
    patch_data = "".join(patch).encode("utf-8")
    manifest = {
        "schema": 1,
        "baseline_description": "Qualified p4_ble_roles snapshot plus isolated peripheral merge; LCD, wheel and three buttons enabled, card disabled",
        "baseline_manifest_sha256": shared.digest((PREVIOUS / "source-manifest.json").read_bytes()),
        "peripheral_manifest_sha256": shared.digest(PERIPHERAL_PROVENANCE.read_bytes()),
        "patch_sha256": shared.digest(patch_data),
        "changes": changes,
        "build_inputs": {name: shared.digest(shared.safe_path(ROOT.parent, name).read_bytes())
                         for name in BUILD_INPUTS},
    }
    (ROOT / "app-overlay.patch").write_bytes(patch_data)
    (ROOT / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return check(destination, manifest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, default=PREVIOUS / "private/app")
    parser.add_argument("--destination", type=Path, default=ROOT / "private/app")
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--check", action="store_true")
    action.add_argument("--refresh", action="store_true")
    args = parser.parse_args()
    try:
        if args.refresh:
            count = refresh(args.baseline, args.destination)
        elif args.check:
            count = check(args.destination, load_manifest())
        else:
            count = prepare(args.baseline, args.destination, load_manifest())
        print(f"Verified {count} source files: {args.destination}")
    except (OSError, ValueError, KeyError, UnicodeError, subprocess.SubprocessError) as error:
        parser.exit(1, f"prepare: {error}\n")


if __name__ == "__main__":
    main()
