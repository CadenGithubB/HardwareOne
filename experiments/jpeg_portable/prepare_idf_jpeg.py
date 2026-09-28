#!/usr/bin/env python3
"""Prepare a verified local IDF 5.5.5 JPEG component override; never edit the SDK.

Only jpeg_common.c allocation/interrupt lifetime and a qualification build flag
change. An existing destination is verified, never overwritten. The override
is optional: regular builds without it retain software decoding.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
SDK_COMMIT = "b774170ff46c393eeb5e495ea37936038d3f4f4f"
SOURCE_TREE_SHA256 = "9d373fba1ee365efcc361f598a769cf2aba28a3aaecc612763028a02259cafbe"
PATCH_SHA256 = "6c344428013de9f9b7f707d35c75ca9390fb3ecdf312fc99fce0174ed811f15f"
PATCHED_COMMON_SHA256 = "f917c427508e8a0d590095edd830351f9a8c15c080bb403f3f5399609ea579cf"
MANIFEST = "hw1-jpeg-qualification.json"
QUALIFICATION_CMAKE = """
# This private override passed the HW1 JPEG allocation-unwind regression.
if(CONFIG_SOC_JPEG_DECODE_SUPPORTED)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC HW1_JPEG_DRIVER_QUALIFIED=1)
endif()
"""


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def files(root: Path, exclude_manifest: bool = False) -> dict[str, str]:
    result = {}
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"unexpected symlink: {path}")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            if exclude_manifest and relative == MANIFEST:
                continue
            result[relative] = digest(path.read_bytes())
    return result


def tree_digest(entries: dict[str, str]) -> str:
    sha = hashlib.sha256()
    for relative, value in sorted(entries.items()):
        sha.update(relative.encode() + b"\0" + bytes.fromhex(value))
    return sha.hexdigest()


def verify_source(idf: Path) -> tuple[Path, dict[str, str], bytes]:
    revision = subprocess.check_output(["git", "-C", str(idf), "rev-parse", "HEAD"], text=True).strip()
    if revision != SDK_COMMIT:
        raise ValueError(f"expected pinned IDF 5.5.5 commit {SDK_COMMIT}, got {revision}")
    source = idf / "components/esp_driver_jpeg"
    entries = files(source)
    if tree_digest(entries) != SOURCE_TREE_SHA256:
        raise ValueError("SDK JPEG component source differs from the reviewed version")
    patch = (ROOT / "idf-jpeg.patch").read_bytes()
    if digest(patch) != PATCH_SHA256:
        raise ValueError("JPEG patch differs from the reviewed version")
    return source, entries, patch


def expected_files(source: Path, entries: dict[str, str]) -> dict[str, str]:
    expected = dict(entries)
    expected["jpeg_common.c"] = PATCHED_COMMON_SHA256
    expected["CMakeLists.txt"] = digest((source / "CMakeLists.txt").read_bytes() + QUALIFICATION_CMAKE.encode())
    return expected


def verify(destination: Path, expected: dict[str, str]) -> None:
    if destination.is_symlink():
        raise ValueError("override destination must not be a symlink")
    manifest = json.loads((destination / MANIFEST).read_text())
    if manifest != {"schema": 1, "sdk_commit": SDK_COMMIT,
                    "source_tree_sha256": SOURCE_TREE_SHA256,
                    "patch_sha256": PATCH_SHA256, "files": expected}:
        raise ValueError("JPEG override provenance mismatch")
    if files(destination, exclude_manifest=True) != expected:
        raise ValueError("JPEG override changed after preparation")


def prepare(idf: Path, destination: Path, check: bool = False) -> Path:
    source, entries, patch = verify_source(idf)
    expected = expected_files(source, entries)
    if destination.exists() or destination.is_symlink():
        verify(destination, expected)
        return destination
    if check:
        raise ValueError("JPEG override has not been prepared")
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".jpeg-idf-", dir=destination.parent))
    try:
        shutil.copytree(source, stage, dirs_exist_ok=True)
        applied = subprocess.run(["patch", "-f", "-F", "0", "-p", "1"], input=patch,
                                 cwd=stage, capture_output=True)
        if applied.returncode:
            raise ValueError("JPEG patch failed: " + applied.stdout.decode() + applied.stderr.decode())
        cmake = stage / "CMakeLists.txt"
        cmake.write_bytes(cmake.read_bytes() + QUALIFICATION_CMAKE.encode())
        if files(stage) != expected:
            raise ValueError("prepared JPEG override does not match reviewed source hashes")
        (stage / MANIFEST).write_text(json.dumps({
            "schema": 1, "sdk_commit": SDK_COMMIT,
            "source_tree_sha256": SOURCE_TREE_SHA256,
            "patch_sha256": PATCH_SHA256, "files": expected,
        }, indent=2, sort_keys=True) + "\n")
        stage.rename(destination)
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    verify(destination, expected)
    return destination


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, default=os.environ.get("IDF_PATH"))
    parser.add_argument("--destination", type=Path,
                        default=ROOT / "private/idf-components/esp_driver_jpeg")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    if args.idf is None:
        parser.error("--idf or IDF_PATH is required")
    try:
        output = prepare(Path(args.idf).resolve(), args.destination.absolute(), args.check)
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"JPEG override: {exc}\n")
    print(f"Verified JPEG override: {output}")


if __name__ == "__main__":
    main()
