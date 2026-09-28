#!/usr/bin/env python3
"""Prepare a new JPEG qualification copy over a verified prior app snapshot.

Normal preparation never replaces a destination. --refresh-hal updates only
the five codec sources in an existing, verified private qualification copy.
Prior experiment snapshots, SDKs and hardware are never modified.
Current JPEG sources and only the G2 Git diff are applied. The three
CMake registration changes are applied by checked, exact anchors; the frozen
profiles already pin esp_jpeg 1.3.1 and retain that dependency. A private
manifest records every changed source hash; --check revalidates before build.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent.parent
COMPONENT = Path("components/hardwareone")
PATCHED_FILES = (str(COMPONENT / "G2_Glasses.cpp"), str(COMPONENT / "CMakeLists.txt"), str(COMPONENT / "idf_component.yml"))
HAL_FILES = tuple(str(COMPONENT / f) for f in (
    "HAL_JPEG.h", "HAL_JPEG_Backend.h", "HAL_JPEG.cpp",
    "HAL_JPEG_Software.cpp", "HAL_JPEG_P4.cpp",
))


def digest(data):
    return hashlib.sha256(data).hexdigest()


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def baseline_definition(target):
    if target == "p4":
        previous = ROOT.parent / "p4_io"
        module = load_module("jpeg_io_prepare", previous / "prepare.py")
        manifest = module.load_manifest()
        expected = module.expected_files(manifest)
        common = module.shared
    else:
        previous = ROOT.parent / "p4_ble_roles"
        module = load_module("jpeg_roles_prepare", previous / "prepare.py")
        manifest = module.load_manifest()
        expected = module.output_files(manifest)
        common = module
    return previous, expected, common


def verify(destination, target, check_worktree=True):
    previous, expected, common = baseline_definition(target)
    manifest = json.loads((destination / "jpeg-qualification.json").read_text())
    if manifest["target"] != target or manifest["schema"] != 1:
        raise ValueError("qualification target/schema mismatch")
    if manifest["baseline_manifest_sha256"] != digest((previous / "source-manifest.json").read_bytes()):
        raise ValueError("baseline manifest changed")
    for rel, sha in manifest["overlay"].items():
        expected[rel] = sha
    common.verify_files(destination, expected, "prepared JPEG source")
    for rel, sha in manifest["worktree_source_sha256"].items():
        if check_worktree and digest((REPO / rel).read_bytes()) != sha:
            raise ValueError(f"JPEG source changed after prepare: {rel}")
    return len(expected)


def prepare(target, baseline_root, destination):
    if destination.exists() or destination.is_symlink():
        raise ValueError(f"destination already exists; choose a new destination or use --check: {destination}")
    previous, expected, common = baseline_definition(target)
    baseline = baseline_root / "experiments" / previous.name / "private/app"
    common.verify_files(baseline, expected, "frozen baseline")
    patch = (ROOT / "integration.patch").read_bytes()
    if not patch:
        raise ValueError("G2/CMake integration patch is missing")
    overlay_bytes = {rel: (REPO / rel).read_bytes() for rel in HAL_FILES}
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".jpeg-prepare-", dir=destination.parent))
    try:
        for relative in expected:
            out = common.safe_path(stage, relative)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(common.safe_path(baseline, relative), out)
        # Reuse downloaded dependencies without sharing writable component dirs.
        # Component Manager validates these against dependencies.lock on build.
        if (baseline / "managed_components").is_dir():
            shutil.copytree(baseline / "managed_components", stage / "managed_components")
        applied = subprocess.run(["patch", "-f", "-F", "0", "-p", "1"], input=patch,
                                 cwd=stage, capture_output=True)
        if applied.returncode:
            raise ValueError("JPEG integration patch failed: " + applied.stdout.decode() + applied.stderr.decode())
        cmake = stage / COMPONENT / "CMakeLists.txt"
        text = cmake.read_text()
        replacements = (
            ("        HAL_Display.cpp\n", "        HAL_Display.cpp\n        HAL_JPEG.cpp\n        HAL_JPEG_Software.cpp\n        HAL_JPEG_P4.cpp\n"),
            ("        esp32-camera\n", "        esp32-camera\n        espressif__esp_jpeg\n"),
            ("idf_component_register(\n", "# Optional hardware JPEG; native S3 retains software decode.\nif(IDF_TARGET STREQUAL \"esp32p4\" OR CONFIG_SOC_JPEG_DECODE_SUPPORTED)\n    list(APPEND hardwareone_requires esp_driver_jpeg)\nendif()\n\nidf_component_register(\n"),
        )
        for before, after in replacements:
            if text.count(before) != 1:
                raise ValueError("JPEG CMake baseline anchor mismatch")
            text = text.replace(before, after)
        cmake.write_text(text)
        dependencies = (stage / COMPONENT / "idf_component.yml").read_text()
        if 'espressif/esp_jpeg:\n    version: "==1.3.1"' not in dependencies:
            raise ValueError("frozen esp_jpeg dependency is not pinned to 1.3.1")
        for rel, data in overlay_bytes.items():
            (stage / rel).write_bytes(data)
        overlay = {rel: digest((stage / rel).read_bytes()) for rel in (*PATCHED_FILES, *HAL_FILES)}
        manifest = {
            "schema": 1, "target": target,
            "baseline": previous.name,
            "baseline_manifest_sha256": digest((previous / "source-manifest.json").read_bytes()),
            "integration_patch_sha256": digest(patch),
            "worktree_source_sha256": {rel: digest((REPO / rel).read_bytes()) for rel in (*PATCHED_FILES, *HAL_FILES)},
            "overlay": overlay,
        }
        (stage / "jpeg-integration.patch").write_bytes(patch)
        (stage / "jpeg-qualification.json").write_text(json.dumps(manifest, indent=2) + "\n")
        count = verify(stage, target)
        stage.rename(destination)
        return count
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def refresh_hal(destination, target):
    verify(destination, target, check_worktree=False)
    manifest_path = destination / "jpeg-qualification.json"
    manifest = json.loads(manifest_path.read_text())
    for rel in PATCHED_FILES:
        if digest((REPO / rel).read_bytes()) != manifest["worktree_source_sha256"][rel]:
            raise ValueError(f"integration source changed; prepare a new destination: {rel}")
    # Only private experiment files are replaced, after all previous content was
    # verified. Do not run this while a build is active against that copy.
    data = {rel: (REPO / rel).read_bytes() for rel in HAL_FILES}
    for rel, content in data.items():
        path = destination / rel
        replacement = path.with_suffix(path.suffix + ".jpeg-new")
        replacement.write_bytes(content)
        replacement.replace(path)
        manifest["overlay"][rel] = digest(content)
        manifest["worktree_source_sha256"][rel] = digest(content)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    return verify(destination, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", choices=("p4", "s3"), required=True)
    parser.add_argument("--baseline-root", type=Path, default=REPO)
    parser.add_argument("--destination", type=Path)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--refresh-hal", action="store_true")
    parser.add_argument("--capture-integration", action="store_true",
                        help="record only the current G2 integration diff, then exit")
    args = parser.parse_args()
    if args.capture_integration:
        patch = subprocess.check_output(["git", "diff", "HEAD", "--", PATCHED_FILES[0]], cwd=REPO)
        if not patch:
            parser.exit(1, "prepare: integration diff is empty\n")
        (ROOT / "integration.patch").write_bytes(patch)
        print("Captured JPEG integration patch")
        return
    destination = args.destination or ROOT / "private" / ("app-" + args.target)
    try:
        if args.refresh_hal:
            count = refresh_hal(destination, args.target)
        elif args.check:
            count = verify(destination, args.target)
        else:
            count = prepare(args.target, args.baseline_root, destination)
        print(f"Verified {count} JPEG qualification sources: {destination}")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"prepare: {error}\n")


if __name__ == "__main__":
    main()
