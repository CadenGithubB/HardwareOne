#!/usr/bin/env python3
"""Prepare a clean ESP-IDF 5.5.5 Bluetooth component with the upstream ECC fix.

Usage: python3 -B prepare_idf_bt.py --idf "$IDF_PATH"
       python3 -B prepare_idf_bt.py --check

The shared SDK is read only. All baseline component files and the SDK version
file are pinned in idf-bt-manifest.json. Only five official TinyCrypt diffs are
applied; the upstream NimBLE submodule update and diagnostic instrumentation are
excluded. Existing output is verified, never overwritten. No build is started.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent
DEFAULT_DESTINATION = ROOT.parent.parent / ".sdk-overrides/esp32p4/bt"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def safe_relative(value: str) -> PurePosixPath:
    relative = PurePosixPath(value)
    if (not value or relative.is_absolute() or ".." in relative.parts
            or "\\" in value or relative.as_posix() != value):
        raise ValueError(f"unsafe manifest path: {value!r}")
    return relative


def check_destination_path(path: Path) -> None:
    # Reject redirects before creating a staging directory. resolve() alone
    # would silently follow a substituted private/idf-components symlink.
    current = path.absolute()
    while current != current.parent:
        if current.is_symlink():
            raise ValueError(f"symlink in destination path: {current}")
        current = current.parent


def scan_tree(root: Path, source_links: dict[str, str] | None = None,
              omit_git: bool = False) -> tuple[dict[str, str], list[str]]:
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"component must be a real directory: {root}")
    allowed = source_links or {}
    seen_links = {}
    files = {}
    executable = []
    for directory, dirs, names in os.walk(root, followlinks=False):
        dirs.sort()
        names.sort()
        for name in list(dirs):
            path = Path(directory) / name
            if omit_git and name == ".git":
                dirs.remove(name)
            elif path.is_symlink():
                raise ValueError(f"directory symlink is unsupported: {path.relative_to(root)}")
        for name in names:
            if omit_git and name == ".git":
                continue
            path = Path(directory) / name
            relative = path.relative_to(root).as_posix()
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode):
                target = os.readlink(path)
                if allowed.get(relative) != target:
                    raise ValueError(f"unrecorded or changed source symlink: {relative}")
                resolved = path.resolve(strict=True)
                if not resolved.is_relative_to(root.resolve()) or not resolved.is_file():
                    raise ValueError(f"source symlink escapes component: {relative}")
                seen_links[relative] = target
                info = resolved.stat()
            if not stat.S_ISREG(info.st_mode):
                raise ValueError(f"nonregular component file: {relative}")
            files[relative] = digest(path.read_bytes())
            if info.st_mode & 0o111:
                executable.append(relative)
    if seen_links != allowed:
        raise ValueError("recorded source symlink is missing or no longer a symlink")
    return dict(sorted(files.items())), sorted(executable)


def verify_tree(root: Path, expected: dict[str, str], executable: list[str],
                source_links: dict[str, str] | None = None,
                omit_git: bool = False) -> int:
    actual, actual_executable = scan_tree(root, source_links, omit_git)
    changed = sorted(name for name in actual.keys() | expected.keys()
                     if actual.get(name) != expected.get(name))
    if changed:
        raise ValueError(f"component content mismatch ({len(changed)} files): "
                         + ", ".join(changed[:8]))
    if actual_executable != executable:
        raise ValueError("component executable file modes changed")
    return len(actual)


def load_manifest(path: Path = ROOT / "idf-bt-manifest.json") -> dict:
    manifest = json.loads(path.read_text())
    if manifest.get("schema") != 1 or manifest.get("idf_version") != "5.5.5":
        raise ValueError("unsupported Bluetooth SDK manifest")
    baseline = manifest["baseline"]
    for relative, sha in baseline["files"].items():
        safe_relative(relative)
        if not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise ValueError(f"invalid baseline hash: {relative}")
    for relative in baseline["source_symlinks"]:
        safe_relative(relative)
        if relative not in baseline["files"]:
            raise ValueError("source symlink is absent from baseline")
    if baseline["executable_files"] != sorted(set(baseline["executable_files"])):
        raise ValueError("invalid executable file list")
    for relative in baseline["executable_files"]:
        if relative not in baseline["files"]:
            raise ValueError("executable file is absent from baseline")
    for relative, sha in manifest["patched_files"].items():
        safe_relative(relative)
        if relative not in baseline["files"] or not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise ValueError("invalid patched file")
    patch = manifest["patch"]
    patch_path = path.parent / safe_relative(patch["file"])
    if patch_path.is_symlink() or digest(patch_path.read_bytes()) != patch["sha256"]:
        raise ValueError("upstream TinyCrypt patch hash mismatch")
    if patch["strip"] != 3:
        raise ValueError("unexpected patch strip level")
    return manifest


def output_files(manifest: dict) -> dict[str, str]:
    files = dict(manifest["baseline"]["files"])
    files.update(manifest["patched_files"])
    return files


def check_output(destination: Path, manifest: dict) -> int:
    check_destination_path(destination)
    return verify_tree(destination, output_files(manifest),
                       manifest["baseline"]["executable_files"])


def check_sdk(idf: Path, manifest: dict) -> int:
    version = manifest["version_file"]
    version_path = idf / safe_relative(version["path"])
    if version_path.is_symlink() or digest(version_path.read_bytes()) != version["sha256"]:
        raise ValueError("SDK version file does not match pinned IDF 5.5.5")
    baseline = manifest["baseline"]
    return verify_tree(idf / "components/bt", baseline["files"],
                       baseline["executable_files"], baseline["source_symlinks"], True)


def prepare(idf: Path, destination: Path, manifest: dict,
            patch_root: Path = ROOT) -> tuple[int, bool]:
    check_destination_path(destination)
    check_sdk(idf, manifest)
    if destination.exists():
        return check_output(destination, manifest), False
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".bt-prepare-", dir=destination.parent))
    try:
        component = idf / "components/bt"
        executable = set(manifest["baseline"]["executable_files"])
        for relative in manifest["baseline"]["files"]:
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            # copyfile follows the one checked internal header link and creates
            # a regular destination file. No SDK/.git references survive.
            shutil.copyfile(component / relative, target)
            target.chmod(0o755 if relative in executable else 0o644)
        verify_tree(stage, manifest["baseline"]["files"], sorted(executable))
        patch_path = patch_root / manifest["patch"]["file"]
        if patch_path.is_symlink() or digest(patch_path.read_bytes()) != manifest["patch"]["sha256"]:
            raise ValueError("upstream TinyCrypt patch changed during preparation")
        result = subprocess.run(
            ["patch", "--verbose", "-f", "-F", "0", "-p", "3", "-i", str(patch_path.resolve())],
            cwd=stage, capture_output=True, text=True, timeout=30,
            env={**os.environ, "LC_ALL": "C"},
        )
        report = result.stdout + result.stderr
        if result.returncode or re.search(r"\b(?:fuzz|offset)\b", report, re.IGNORECASE):
            raise ValueError("upstream patch did not apply exactly at recorded positions")
        count = check_output(stage, manifest)
        # Detect an output created while we staged. Never merge or replace it.
        if destination.exists() or destination.is_symlink():
            raise ValueError("destination appeared during preparation; refusing replacement")
        stage.rename(destination)
        return count, True
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"),
                        help="read-only IDF 5.5.5 checkout; defaults to IDF_PATH")
    parser.add_argument("--destination", type=Path, default=DEFAULT_DESTINATION)
    parser.add_argument("--check", action="store_true", help="verify exact output without copying or patching")
    args = parser.parse_args()
    try:
        manifest = load_manifest()
        if args.check:
            count = check_output(args.destination, manifest)
            action = "Verified"
        else:
            if args.idf is None:
                raise ValueError("provide --idf or export IDF_PATH")
            count, created = prepare(args.idf, args.destination, manifest)
            action = "Prepared" if created else "Verified existing"
        print(f"{action} {count} regular Bluetooth component files: {args.destination}")
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        parser.exit(1, f"prepare_idf_bt: {error}\n")


if __name__ == "__main__":
    main()
