#!/usr/bin/env python3
"""Build or inspect an offline HardwareOne companion-app OTA bundle.

The ``.hw1ota`` container is deliberately small and boring: a deterministic
ZIP with one signed manifest, its exact ESP-IDF image, and the public key that
lets an offline companion fail fast before transferring several megabytes.
The device still verifies the manifest against its own embedded trust root and
independently validates the ESP-IDF signed image before selecting it.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import sys
import zipfile

if __package__ in (None, ""):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from tools.ota import make_manifest


BUNDLE_FORMAT = "hardwareone-ota-bundle"
BUNDLE_FORMAT_VERSION = 1
BUNDLE_METADATA = "bundle.json"
BUNDLE_MANIFEST = "manifest.json"
BUNDLE_IMAGE = "firmware.bin"
BUNDLE_PUBLIC_KEY = "public-key.pem"
BUNDLE_MEMBERS = (
    BUNDLE_METADATA,
    BUNDLE_MANIFEST,
    BUNDLE_PUBLIC_KEY,
    BUNDLE_IMAGE,
)
MAX_MANIFEST_SIZE = 2048
MAX_IMAGE_SIZE = 0x5A0000
FIXED_ZIP_TIME = (1980, 1, 1, 0, 0, 0)


def _manifest_parts_from_bytes(raw: bytes) -> tuple[bytes, bytes, dict[str, object]]:
    if not raw or len(raw) > MAX_MANIFEST_SIZE:
        raise ValueError(
            f"manifest size must be 1..{MAX_MANIFEST_SIZE} bytes, got {len(raw)}"
        )
    try:
        envelope = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError("manifest is not valid UTF-8 JSON") from exc
    if (
        envelope.get("format") != make_manifest.FORMAT
        or envelope.get("formatVersion") != make_manifest.FORMAT_VERSION
    ):
        raise ValueError("unsupported OTA manifest envelope")
    signature = envelope.get("signature")
    if not isinstance(signature, dict) or signature.get("algorithm") != "rsa-pss-sha256":
        raise ValueError("unsupported OTA manifest signature")
    try:
        payload = base64.b64decode(envelope["payload"], validate=True)
        signed = base64.b64decode(signature["value"], validate=True)
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("manifest payload/signature is missing or invalid base64") from exc
    if len(signed) != make_manifest.SIGNATURE_SIZE:
        raise ValueError("manifest signature is not RSA-3072")
    return payload, signed, make_manifest.decode_payload(payload)


def _manifest_parts(path: pathlib.Path) -> tuple[bytes, bytes, dict[str, object]]:
    return _manifest_parts_from_bytes(path.read_bytes())


def verify_pair(
    image: pathlib.Path,
    manifest: pathlib.Path,
    public_key: pathlib.Path,
) -> dict[str, object]:
    """Verify the detached signature and exact image/descriptor identity."""
    for label, path in (
        ("image", image),
        ("manifest", manifest),
        ("public key", public_key),
    ):
        if not path.is_file():
            raise ValueError(f"{label} not found: {path}")
    size = image.stat().st_size
    if size <= 0 or size > MAX_IMAGE_SIZE:
        raise ValueError(f"image size {size} does not fit ota_0 ({MAX_IMAGE_SIZE} bytes)")
    payload, signature, fields = _manifest_parts(manifest)
    make_manifest.verify_payload(payload, signature, public_key)
    project, version = make_manifest.read_app_descriptor(image)
    actual = {
        "imageSha256": make_manifest.sha256_file(image),
        "imageSize": size,
        "projectName": project,
        "version": version,
    }
    mismatches = [
        f"{key}: manifest={fields.get(key)!r}, actual={value!r}"
        for key, value in actual.items()
        if fields.get(key) != value
    ]
    if mismatches:
        raise ValueError("manifest/image mismatch: " + "; ".join(mismatches))
    return fields


def _zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, FIXED_ZIP_TIME)
    info.compress_type = zipfile.ZIP_STORED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    return info


def create_bundle(
    image: pathlib.Path,
    manifest: pathlib.Path,
    public_key: pathlib.Path,
    output: pathlib.Path,
) -> dict[str, object]:
    image = image.resolve()
    manifest = manifest.resolve()
    public_key = public_key.resolve()
    fields = verify_pair(image, manifest, public_key)
    if output.suffix.lower() != ".hw1ota":
        raise ValueError("bundle output must use the .hw1ota extension")
    output.parent.mkdir(parents=True, exist_ok=True)
    metadata = {
        "format": BUNDLE_FORMAT,
        "formatVersion": BUNDLE_FORMAT_VERSION,
        "firmware": BUNDLE_IMAGE,
        "manifest": BUNDLE_MANIFEST,
        "publicKey": BUNDLE_PUBLIC_KEY,
    }
    entries = (
        (BUNDLE_METADATA, json.dumps(metadata, separators=(",", ":"), sort_keys=True).encode("utf-8") + b"\n"),
        (BUNDLE_MANIFEST, manifest.read_bytes()),
        (BUNDLE_PUBLIC_KEY, public_key.read_bytes()),
        (BUNDLE_IMAGE, image.read_bytes()),
    )
    with zipfile.ZipFile(output, "w", allowZip64=False) as bundle:
        for name, value in entries:
            bundle.writestr(_zip_info(name), value)
    return {
        "board": fields["boardId"],
        "layout": fields["layoutId"],
        "version": fields["version"],
        "imageSize": fields["imageSize"],
        "imageSha256": fields["imageSha256"],
        "bundleSha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "output": str(output),
    }


def inspect_bundle(path: pathlib.Path) -> dict[str, object]:
    if not path.is_file():
        raise ValueError(f"bundle not found: {path}")
    with zipfile.ZipFile(path, "r") as bundle:
        names = tuple(info.filename for info in bundle.infolist())
        if names != BUNDLE_MEMBERS or len(set(names)) != len(names):
            raise ValueError(
                "bundle members/order must be exactly " + ", ".join(BUNDLE_MEMBERS)
            )
        metadata = json.loads(bundle.read(BUNDLE_METADATA).decode("utf-8"))
        if (
            metadata.get("format") != BUNDLE_FORMAT
            or metadata.get("formatVersion") != BUNDLE_FORMAT_VERSION
        ):
            raise ValueError("unsupported HardwareOne OTA bundle")
        for info in bundle.infolist():
            if info.compress_type != zipfile.ZIP_STORED:
                raise ValueError("bundle entries must be stored without ZIP compression")
        if bundle.getinfo(BUNDLE_MANIFEST).file_size > MAX_MANIFEST_SIZE:
            raise ValueError("manifest exceeds the recovery limit")
        if not 0 < bundle.getinfo(BUNDLE_IMAGE).file_size <= MAX_IMAGE_SIZE:
            raise ValueError("firmware image exceeds ota_0")
        payload, _signature, fields = _manifest_parts_from_bytes(
            bundle.read(BUNDLE_MANIFEST)
        )
        if len(payload) != make_manifest.PAYLOAD_SIZE:
            raise ValueError("manifest payload has the wrong size")
        image_digest = hashlib.sha256(bundle.read(BUNDLE_IMAGE)).hexdigest()
        if fields["imageSize"] != bundle.getinfo(BUNDLE_IMAGE).file_size:
            raise ValueError("bundle firmware size does not match manifest")
        if fields["imageSha256"] != image_digest:
            raise ValueError("bundle firmware digest does not match manifest")
    return {
        "board": fields["boardId"],
        "layout": fields["layoutId"],
        "version": fields["version"],
        "imageSize": fields["imageSize"],
        "imageSha256": fields["imageSha256"],
        "bundleSha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }
def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)
    create = sub.add_parser("create", help="verify a release pair and create a .hw1ota bundle")
    create.add_argument("--image", type=pathlib.Path, required=True)
    create.add_argument("--manifest", type=pathlib.Path, required=True)
    create.add_argument("--public-key", type=pathlib.Path, required=True)
    create.add_argument("--output", type=pathlib.Path, required=True)
    inspect = sub.add_parser("inspect", help="validate and describe a .hw1ota bundle")
    inspect.add_argument("bundle", type=pathlib.Path)
    return root


def main() -> None:
    args = parser().parse_args()
    try:
        if args.command == "create":
            result = create_bundle(args.image, args.manifest, args.public_key, args.output)
        else:
            result = inspect_bundle(args.bundle)
    except (ValueError, zipfile.BadZipFile) as exc:
        raise SystemExit(f"invalid OTA bundle: {exc}") from exc
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
