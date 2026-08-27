#!/usr/bin/env python3
"""Generate deterministic, lab-signed negative HardwareOne OTA fixtures.

The output is intentionally separate from release artifacts. The private lab
key is read but never copied or named in the generated index.
"""

from __future__ import annotations

import argparse
import base64
import json
import pathlib
import shutil
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from tools.ota import make_manifest
from tools.ota.qualification.artifacts import public_key_fingerprint, sha256


INDEX_FORMAT = "hardwareone-ota-negative-fixtures"
INDEX_VERSION = 1


def canonical_fields(image: pathlib.Path, board: str) -> dict[str, object]:
    project, version = make_manifest.read_app_descriptor(image)
    if project != "hardwareone-idf":
        raise ValueError(f"unexpected app project {project!r}")
    suffix = make_manifest.BOARD_SUFFIXES[board]
    if not version.endswith(suffix):
        raise ValueError(
            f"image version {version!r} does not belong to board {board!r}"
        )
    return {
        "boardId": board,
        "dataSchema": 1,
        "imageSha256": sha256(image),
        "imageSize": image.stat().st_size,
        "layoutId": make_manifest.BOARD_LAYOUTS[board],
        "minUpdaterVersion": "1.0.0",
        "projectName": project,
        "version": version,
    }


def mutate_manifest_signature(source: pathlib.Path, output: pathlib.Path) -> None:
    value = json.loads(source.read_text(encoding="utf-8"))
    signature = bytearray(
        base64.b64decode(value["signature"]["value"], validate=True)
    )
    if len(signature) != make_manifest.SIGNATURE_SIZE:
        raise ValueError("control manifest does not contain an RSA-3072 signature")
    signature[0] ^= 0x01
    value["signature"]["value"] = base64.b64encode(signature).decode("ascii")
    output.write_text(
        json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def write_mutated_image(
    source: pathlib.Path, output: pathlib.Path, *, offset: int
) -> None:
    content = bytearray(source.read_bytes())
    if not 0 <= offset < len(content):
        raise ValueError("image mutation offset is outside the image")
    content[offset] ^= 0x01
    output.write_bytes(content)


def fixture_record(
    root: pathlib.Path,
    fixture_id: str,
    image: pathlib.Path | None,
    manifest: pathlib.Path | None,
    expected_layer: str,
    description: str,
    *,
    destructive: bool = False,
) -> dict[str, object]:
    return {
        "id": fixture_id,
        "image": None if image is None else image.relative_to(root).as_posix(),
        "manifest": None if manifest is None else manifest.relative_to(root).as_posix(),
        "expectedRejectionLayer": expected_layer,
        "description": description,
        "destructive": destructive,
        "imageSha256": None if image is None else sha256(image),
        "manifestSha256": None if manifest is None else sha256(manifest),
    }


def generate(args: argparse.Namespace) -> pathlib.Path:
    source = args.image.resolve()
    key = args.lab_key.resolve()
    output = args.output.resolve()
    if not args.acknowledge_lab_key:
        raise ValueError("--acknowledge-lab-key is required")
    if not source.is_file():
        raise ValueError(f"image not found: {source}")
    if not key.is_file():
        raise ValueError(f"lab key not found: {key}")
    if output.exists():
        raise ValueError(f"fixture output already exists: {output}")
    fields = canonical_fields(source, args.board)

    images = output / "images"
    manifests = output / "manifests"
    images.mkdir(parents=True)
    manifests.mkdir(parents=True)
    good_image = images / "good.bin"
    shutil.copyfile(source, good_image)
    good_manifest = manifests / "valid-control.json"
    make_manifest.write_envelope(fields, key, good_manifest)

    public_key = output / "lab-public-key.pem"
    make_manifest.extract_public_key(key, public_key)

    truncated_image = images / "truncated.bin"
    truncated_image.write_bytes(source.read_bytes()[: source.stat().st_size // 2])
    same_size_corrupt = images / "same-size-corrupt.bin"
    payload_offset = max(
        make_manifest.APP_DESC_OFFSET + 256,
        min(source.stat().st_size // 2, source.stat().st_size - 1),
    )
    write_mutated_image(source, same_size_corrupt, offset=payload_offset)
    native_invalid = images / "native-signature-invalid.bin"
    # Damage authenticated application payload, not trailing signature padding.
    # A freshly signed detached manifest will then pass while ESP-IDF's native
    # signed-image verification must reject the changed application.
    write_mutated_image(source, native_invalid, offset=payload_offset)

    native_fields = dict(fields)
    native_fields["imageSha256"] = sha256(native_invalid)
    native_manifest = manifests / "native-signature-invalid.json"
    make_manifest.write_envelope(native_fields, key, native_manifest)

    invalid_signature = manifests / "invalid-signature.json"
    mutate_manifest_signature(good_manifest, invalid_signature)

    # Any board that is not this one works as the negative case; picking it
    # from the table means a newly added board is covered automatically instead
    # of falling through a two-way ternary.
    other_board = next(
        board for board in sorted(make_manifest.BOARD_LAYOUTS)
        if board != args.board
    )
    variants: dict[str, dict[str, object]] = {}
    wrong_board = dict(fields)
    wrong_board["boardId"] = other_board
    wrong_board["layoutId"] = make_manifest.BOARD_LAYOUTS[other_board]
    variants["wrong-board"] = wrong_board
    wrong_layout = dict(fields)
    wrong_layout["layoutId"] = "hw1-invalid-layout-v1"
    variants["wrong-layout"] = wrong_layout
    unsupported_schema = dict(fields)
    unsupported_schema["dataSchema"] = 2
    variants["unsupported-schema"] = unsupported_schema
    minimum_too_new = dict(fields)
    minimum_too_new["minUpdaterVersion"] = "9999.0.0"
    variants["min-updater-too-new"] = minimum_too_new
    wrong_size = dict(fields)
    wrong_size["imageSize"] = int(fields["imageSize"]) + 1
    variants["wrong-size"] = wrong_size
    wrong_digest = dict(fields)
    wrong_digest["imageSha256"] = "01" * 32
    variants["wrong-digest"] = wrong_digest
    variant_paths: dict[str, pathlib.Path] = {}
    for name, variant_fields in variants.items():
        path = manifests / f"{name}.json"
        make_manifest.write_envelope(variant_fields, key, path)
        variant_paths[name] = path

    truncated_manifest = manifests / "truncated.json"
    control_bytes = good_manifest.read_bytes()
    truncated_manifest.write_bytes(control_bytes[: len(control_bytes) // 2])
    malformed_manifest = manifests / "malformed-envelope.json"
    malformed_manifest.write_bytes(b'{"format":"hardwareone-ota-envelope"')
    oversized_manifest = manifests / "oversized-envelope.json"
    if len(control_bytes) >= 2049:
        raise ValueError("control manifest unexpectedly exceeds the v1 size boundary")
    oversized_manifest.write_bytes(control_bytes + b" " * (2049 - len(control_bytes)))

    fixtures = [
        fixture_record(output, "valid-control", good_image, good_manifest, "none", "valid lab-signed control pair"),
        fixture_record(output, "candidate-only", good_image, None, "main-stage", "candidate part exists without manifest part"),
        fixture_record(output, "manifest-only", None, good_manifest, "main-stage", "manifest part exists without candidate part"),
        fixture_record(output, "truncated-image", truncated_image, good_manifest, "main-stage", "candidate is shorter than signed size"),
        fixture_record(output, "truncated-manifest", good_image, truncated_manifest, "main-stage", "manifest JSON/base64 is truncated"),
        fixture_record(output, "same-size-digest-mismatch", same_size_corrupt, good_manifest, "main-stage", "same-size candidate digest differs from signed digest"),
        fixture_record(output, "invalid-manifest-signature", good_image, invalid_signature, "manifest-signature", "one signature bit is changed"),
        fixture_record(output, "native-signature-invalid", native_invalid, native_manifest, "esp_ota_end", "detached manifest matches but native ESP app signature is damaged", destructive=True),
        fixture_record(output, "wrong-board", good_image, variant_paths["wrong-board"], "identity-policy", "signed board/layout belongs to the other board"),
        fixture_record(output, "wrong-layout", good_image, variant_paths["wrong-layout"], "identity-policy", "signed layout is unsupported"),
        fixture_record(output, "unsupported-schema", good_image, variant_paths["unsupported-schema"], "compatibility-policy", "signed data schema is unsupported"),
        fixture_record(output, "min-updater-too-new", good_image, variant_paths["min-updater-too-new"], "compatibility-policy", "signed minimum updater is deliberately too new"),
        fixture_record(output, "wrong-size", good_image, variant_paths["wrong-size"], "candidate-policy", "signed size differs by one byte"),
        fixture_record(output, "wrong-digest", good_image, variant_paths["wrong-digest"], "candidate-policy", "signed digest does not match the image"),
        fixture_record(output, "malformed-envelope", good_image, malformed_manifest, "manifest-parser", "manifest is incomplete JSON"),
        fixture_record(output, "oversized-envelope", good_image, oversized_manifest, "manifest-size", "manifest is exactly 2049 bytes"),
    ]
    index = {
        "format": INDEX_FORMAT,
        "formatVersion": INDEX_VERSION,
        "board": args.board,
        "layout": make_manifest.BOARD_LAYOUTS[args.board],
        "sourceImageSha256": sha256(source),
        "labPublicKeyFingerprint": public_key_fingerprint(public_key),
        "fixtures": fixtures,
    }
    (output / "fixture-index.json").write_text(
        json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return output


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    root.add_argument("--image", type=pathlib.Path, required=True)
    root.add_argument("--lab-key", type=pathlib.Path, required=True)
    root.add_argument("--output", type=pathlib.Path, required=True)
    root.add_argument(
        "--board", choices=sorted(make_manifest.BOARD_LAYOUTS), required=True
    )
    root.add_argument(
        "--acknowledge-lab-key",
        action="store_true",
        help="confirm that --lab-key is a non-production key for disposable hardware",
    )
    return root


def main() -> int:
    try:
        output = generate(parser().parse_args())
    except (OSError, ValueError) as exc:
        print(f"fixture generation failed: {exc}", file=sys.stderr)
        return 2
    print(f"generated lab-only OTA fixtures: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
