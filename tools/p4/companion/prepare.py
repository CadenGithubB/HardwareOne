#!/usr/bin/env python3
"""Prepare the ESP32-C6 companion firmware sources; --check validates only.

Fetches the pinned ESP-Hosted MCU slave and esp-serial-flasher into the ignored
private/ workspace, applies the recorded flasher patch, and generates the C6
slave project (pinned Hosted slave + HardwareOne ESP-NOW bridge) under
private/c6-slave/. The pins come from
components/esp_now_hosted/include/esp_now_hosted_companion.h, the same header
the P4 firmware checks against at boot, and the bridge sources are the tracked
bridge/esp_now_hosted_slave.* plus the host component's esp_now_hosted_rpc.h.

No ESP-IDF build or serial-device access is performed here. Existing git caches
must have the exact pinned HEAD. A manifest protects the generated C6 source
from accidental overwrites while allowing the checked-in bridge to be refreshed.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
REPOSITORY = ROOT.parents[2]
PRIVATE = ROOT / "private"
COMPANION_HEADER = REPOSITORY / "components/esp_now_hosted/include/esp_now_hosted_companion.h"
HOST_INCLUDE = REPOSITORY / "components/esp_now_hosted/include"
MANIFEST = ".hw1-prepare.json"
PATCH_FILES = {"src/esp_loader.c", "src/slip.c"}
# Tracked bridge inputs: the slave side lives here, the wire protocol header
# is the host component's copy so the two ends cannot drift apart.
BRIDGE_SOURCES = {
    "esp_now_hosted_slave.c": ROOT / "bridge/esp_now_hosted_slave.c",
    "esp_now_hosted_slave.h": ROOT / "bridge/esp_now_hosted_slave.h",
    "esp_now_hosted_rpc.h": HOST_INCLUDE / "esp_now_hosted_rpc.h",
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def companion_pins() -> dict[str, str]:
    """Read the string literals from the companion header as text."""
    text = COMPANION_HEADER.read_text()
    pins = {}
    for key in ("HW1_C6_HOSTED_TAG", "HW1_C6_HOSTED_SHA",
                "HW1_C6_FLASHER_TAG", "HW1_C6_FLASHER_SHA"):
        match = re.search(r'^#define\s+' + key + r'\s+"([^"]+)"\s*$', text, re.M)
        if not match:
            fail(f"{COMPANION_HEADER} does not define {key} as a string literal")
        pins[key] = match.group(1)
    return pins


PINS = companion_pins()
HOSTED_SHA = PINS["HW1_C6_HOSTED_SHA"]
FLASHER_SHA = PINS["HW1_C6_FLASHER_SHA"]


def c6_dependency_lock() -> bytes:
    """Expand only the portable local SDK path; retain all resolved versions/hashes."""
    configured = os.environ.get("IDF_PATH")
    if not configured:
        fail("Export ESP-IDF 5.5.5 (IDF_PATH) before preparing or checking the C6 dependency lock.")
    idf = Path(configured).resolve()
    version_file = idf / "tools/cmake/version.cmake"
    if not version_file.is_file():
        fail(f"IDF_PATH does not identify an ESP-IDF installation: {idf}")
    version = version_file.read_text()
    values = [re.search(r"set\(IDF_VERSION_" + key + r"\s+(\d+)\)", version)
              for key in ("MAJOR", "MINOR", "PATCH")]
    if any(value is None for value in values) or tuple(int(value.group(1)) for value in values) != (5, 5, 5):
        fail("The C6 dependency lock requires ESP-IDF 5.5.5.")
    if not (idf / "examples/system/console/advanced/components/cmd_system").is_dir():
        fail("The exported SDK is missing the cmd_system example component required by Hosted.")
    template = (ROOT / "dependency_locks/c6.lock").read_text()
    if template.count("${IDF_PATH}") != 1:
        fail("The C6 lock template must contain exactly one portable SDK path.")
    return template.replace("${IDF_PATH}", str(idf)).encode()


def run(*args: str, cwd: Path | None = None) -> bytes:
    return subprocess.check_output(args, cwd=cwd, stderr=subprocess.PIPE)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def ensure_repo(name: str, tag: str, sha: str, check: bool, allowed_changes=()) -> Path:
    directory = PRIVATE / name
    url = f"https://github.com/espressif/{name}.git"
    if directory.is_symlink():
        fail(f"Refusing symlink dependency directory: {directory}")
    if not directory.exists():
        if check:
            fail(f"Missing {directory}; run prepare.py first.")
        # Clone into a new temporary directory. A failed fetch never leaves a
        # half-created cache with the authoritative dependency name.
        with tempfile.TemporaryDirectory(prefix=f".{name}-", dir=PRIVATE) as temporary:
            staging = Path(temporary) / "checkout"
            print(f"Fetching {name} {tag} ({sha})", flush=True)
            subprocess.run(["git", "clone", "--depth", "1", "--branch", tag,
                            url, str(staging)], check=True)
            if run("git", "rev-parse", "HEAD", cwd=staging).decode().strip() != sha:
                fail(f"{tag} no longer resolves to the pinned SHA; refusing it.")
            subprocess.run(["git", "checkout", "--detach", sha], cwd=staging, check=True)
            staging.rename(directory)
    if not directory.is_dir() or not (directory / ".git").exists():
        fail(f"Existing {directory} is not the expected git cache; leave it untouched.")
    toplevel = Path(run("git", "rev-parse", "--show-toplevel", cwd=directory).decode().strip())
    if toplevel.resolve() != directory.resolve():
        fail(f"{directory} is not its own repository.")
    if run("git", "rev-parse", "HEAD", cwd=directory).decode().strip() != sha:
        fail(f"{directory} has a different HEAD; expected {sha}. No checkout was changed.")
    origin = run("git", "remote", "get-url", "origin", cwd=directory).decode().strip()
    if origin.rstrip("/") not in (url, url.removesuffix(".git")):
        fail(f"{directory} has an unexpected origin; refusing to reuse it.")
    changed = set(run("git", "diff", "--name-only", "HEAD", cwd=directory).decode().splitlines())
    untracked = run("git", "ls-files", "--others", "--exclude-standard", cwd=directory)
    if changed - set(allowed_changes) or untracked.strip():
        fail(f"{directory} contains unexpected edits or untracked files; preserve them before retrying.")
    print(f"Verified {name} HEAD {sha}")
    return directory


def prepare_flasher(directory: Path, check: bool) -> None:
    patch = ROOT / "dependency_patches" / "serial-flasher-1.10.0.patch"
    baseline = {name: run("git", "show", f"HEAD:{name}", cwd=directory) for name in PATCH_FILES}
    # Evaluate the exact checked-in patch against pristine files in a temporary
    # directory, then compare bytes before modifying anything in the cache.
    with tempfile.TemporaryDirectory(prefix="hw1-flasher-patch-") as temporary:
        staging = Path(temporary)
        for name, data in baseline.items():
            (staging / name).parent.mkdir(parents=True, exist_ok=True)
            (staging / name).write_bytes(data)
        subprocess.run(["git", "apply", str(patch)], cwd=staging, check=True)
        expected = {name: (staging / name).read_bytes() for name in PATCH_FILES}
    pending = []
    for name, data in expected.items():
        destination = directory / name
        if destination.is_symlink() or not destination.is_file():
            fail(f"Unexpected dependency file: {destination}")
        actual = destination.read_bytes()
        if actual not in (baseline[name], data):
            fail(f"Refusing to overwrite unrelated changes in {destination}")
        if actual != data:
            pending.append((destination, data))
    if check and pending:
        fail("The serial-flasher compatibility patch is not fully applied; run prepare.py.")
    for destination, data in pending:
        destination.write_bytes(data)
    print("Verified serial-flasher 1.10.0 compatibility patch" if not pending
          else "Applied serial-flasher 1.10.0 compatibility patch")


def slave_sources(hosted: Path, dependency_lock: bytes) -> tuple[dict[str, bytes], dict[str, bytes]]:
    baseline = {}
    for name in run("git", "ls-files", "slave", "common", cwd=hosted).decode().splitlines():
        source = hosted / name
        if not source.is_file():  # Uninitialized protobuf-c submodule; IDF resolves its component.
            continue
        if source.is_symlink():
            fail(f"Unexpected symlink in pinned hosted source: {source}")
        destination = name[6:] if name.startswith("slave/") else "main/" + name
        baseline[destination] = source.read_bytes()
    expected = dict(baseline)
    cmake = expected["main/CMakeLists.txt"].decode()
    anchor = "register_component()"
    if cmake.count(anchor) != 1:
        fail("Pinned slave CMakeLists no longer has the expected registration anchor.")
    cmake = cmake.replace(anchor,
                         "list(APPEND COMPONENT_SRCS esp_now_hosted_slave.c)\n"
                         "register_component()\n"
                         'target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,esp_now_hosted_slave_init")')
    expected["main/CMakeLists.txt"] = cmake.encode()
    expected["sdkconfig.defaults"] += (
        b"\nCONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y\n"
        b"CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8\n")
    for name, source in BRIDGE_SOURCES.items():
        expected["main/" + name] = source.read_bytes()
    expected["dependencies.lock"] = dependency_lock
    return baseline, expected


def generated_path(name: str) -> bool:
    return name in (MANIFEST, "sdkconfig", "sdkconfig.old", "dependencies.lock") or \
        name.startswith(("build/", "managed_components/"))


def prepare_slave(hosted: Path, dependency_lock: bytes, check: bool) -> None:
    destination = PRIVATE / "c6-slave"
    baseline, expected = slave_sources(hosted, dependency_lock)
    previous = None
    if destination.is_symlink() or (destination.exists() and not destination.is_dir()):
        fail(f"Refusing unexpected C6 source destination: {destination}")
    marker = destination / MANIFEST
    if marker.is_symlink():
        fail(f"Refusing symlink preparation manifest: {marker}")
    if marker.exists():
        previous = json.loads(marker.read_text())
        if previous.get("hosted_sha") != HOSTED_SHA or previous.get("format") != 1:
            fail(f"Unrecognized generated-source manifest: {marker}")
        if not isinstance(previous.get("files"), dict):
            fail(f"Malformed generated-source manifest: {marker}")
    elif destination.exists():
        # Adopt the initial hand-prepared experiment only after all original
        # source files and overlays have been identified by their exact bytes.
        if not (destination / "CMakeLists.txt").is_file():
            fail(f"Unknown existing directory: {destination}; no files overwritten.")
        print("Checking unmarked C6 source against the pinned source and bridge")
    elif check:
        fail(f"Missing {destination}; run prepare.py first.")

    if destination.exists():
        for path in destination.rglob("*"):
            name = path.relative_to(destination).as_posix()
            if generated_path(name):
                continue
            if path.is_symlink():
                fail(f"Refusing symlink in generated source: {path}")
            if path.is_file() and name not in expected:
                fail(f"Unrecognized file in C6 source: {path}; no files overwritten.")
    pending = []
    for name, data in expected.items():
        path = destination / name
        if path.exists():
            if not path.is_file() or path.is_symlink():
                fail(f"Unexpected generated-source entry: {path}")
            actual = path.read_bytes()
            if name == "dependencies.lock" and actual != data:
                fail(f"Preserving changed {path}; it differs from the durable C6 lock for this SDK.")
            if previous:
                recognized = digest(actual) in (previous["files"].get(name), digest(data))
            else:
                recognized = actual == data or actual == baseline.get(name)
            if not recognized:
                fail(f"Refusing to overwrite unknown edits in {path}")
            if actual == data:
                continue
        elif previous and name in previous["files"] and name != "dependencies.lock":
            fail(f"Previously managed source was removed: {path}; inspect before recreating it.")
        elif destination.exists() and name in baseline:
            fail(f"Incomplete unmarked C6 source: {path}; no files overwritten.")
        pending.append((path, data))
    if check and pending:
        fail("C6 source needs preparation/bridge refresh; run prepare.py.")
    for path, data in pending:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    if not check:
        manifest = {"format": 1, "hosted_sha": HOSTED_SHA,
                    "files": {name: digest(data) for name, data in sorted(expected.items())}}
        marker.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified C6 source ({len(expected)} managed files; {len(pending)} updated)")
    config = destination / "sdkconfig"
    if config.exists():
        text = config.read_text()
        for setting in ("CONFIG_ESP_HOSTED_ENABLE_PEER_DATA_TRANSFER=y",
                        "CONFIG_ESP_HOSTED_MAX_CUSTOM_MSG_HANDLERS=8"):
            if setting not in text.splitlines():
                fail(f"Preserved {config} lacks {setting}; update its configuration before building.")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="offline verification; no dependency/source writes")
    args = parser.parse_args()
    dependency_lock = c6_dependency_lock()
    if PRIVATE.is_symlink():
        fail(f"Refusing symlink private workspace: {PRIVATE}")
    if not args.check:
        PRIVATE.mkdir(exist_ok=True)
    hosted = ensure_repo("esp-hosted-mcu", PINS["HW1_C6_HOSTED_TAG"], HOSTED_SHA, args.check)
    flasher = ensure_repo("esp-serial-flasher", PINS["HW1_C6_FLASHER_TAG"], FLASHER_SHA,
                          args.check, PATCH_FILES)
    prepare_flasher(flasher, args.check)
    prepare_slave(hosted, dependency_lock, args.check)
    print("Companion firmware sources ready. No build or device access performed.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"prepare.py: {error}", file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError) and error.stderr:
            print(error.stderr.decode(errors="replace"), file=sys.stderr)
        sys.exit(1)
