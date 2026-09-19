#!/usr/bin/env python3
"""Create or verify a HardwareOne OTA artifact manifest.

The manifest authenticates the transfer contract (size, SHA-256, board/layout
identity and compatibility fields).  The ESP application remains independently
signed in ESP-IDF's native Secure-Boot-v2 signature-block format; both checks are
required by the device.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

if __package__ in (None, ""):
    sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2]))

from tools.ota import deployment_contract, semver


FORMAT = "hardwareone-ota-envelope"
FORMAT_VERSION = 1
APP_DESC_OFFSET = 24 + 8
APP_DESC_MAGIC = 0xABCD5432
PAYLOAD_MAGIC = 0x314D3148  # "H1M1", little-endian
PAYLOAD_SIZE = 224
SIGNATURE_SIZE = 384
# One row per recovery-OTA board. Keep in step with the registry in the root
# CMakeLists.txt, BOARD_CONTRACT in check_ota_builds.py, and the id constants in
# components/hw1_ota_protocol/include/hw1_ota_protocol.h.
BOARD_LAYOUTS = {
    "feathers3": "hw1-f3-ota-v1",
    "feathers3_fe": "hw1-f3fe-ota-v1",
    "feather_esp32_v2": "hw1-fv2-ota-v1",
    "qtpy_esp32": "hw1-qtpy-ota-v1",
}

# The build-metadata suffix each board's images must carry. This used to be
# written inline as `"+f3feo1" if board == "feathers3_fe" else "+f3o1"` in three
# separate files, which silently assigned the plain-FeatherS3 suffix to every
# board that was not the flash-encrypted one -- so a third board would have been
# validated against the wrong suffix rather than rejected.
# Which partition CSV defines each board's slot sizes. Mirrors BOARD_CONTRACT in
# check_ota_builds.py; both read the CSV rather than restating the numbers.
BOARD_PARTITION_CSV = {
    "feathers3": "partitions_ota_no_sr_16mb.csv",
    "feathers3_fe": "partitions_ota_no_sr_16mb.csv",
    "feather_esp32_v2": "partitions_ota_no_sr_8mb.csv",
    "qtpy_esp32": "partitions_ota_no_sr_8mb.csv",
}

BOARD_SUFFIXES = {
    "feathers3": "+f3o1",
    "feathers3_fe": "+f3feo1",
    "feather_esp32_v2": "+fv2o1",
    "qtpy_esp32": "+qtpyo1",
}


def selectable_boards() -> tuple[str, ...]:
    """Board ids a --board argument may name.

    The legacy registries above cover boards with a board-only recovery layout.
    A board can also arrive purely through a deployment contract (xiao_s3 has
    no board-only layout and is reachable only as pocket_assistant/xiao_s3), so
    those are unioned in rather than requiring a fictitious legacy row.
    """
    return tuple(sorted(set(BOARD_LAYOUTS) | set(deployment_contract.board_ids())))


def resolve_contract(board: str, deployment: str | None = None) -> dict[str, object]:
    """Resolve legacy board-only or deployment-specific OTA identity."""
    if deployment:
        selected = deployment_contract.load(deployment)
        if selected.board_id != board:
            raise ValueError(
                f"deployment {deployment!r} is for {selected.board_id}, not {board}"
            )
        return {
            "board": selected.board_id,
            "layout": selected.layout_id,
            "suffix": selected.version_suffix,
            "partition_csv": selected.partition_csv,
            "slot_size": selected.ota_slot_size,
            "main_release_max": selected.main_release_max,
        }
    if board not in BOARD_LAYOUTS:
        raise ValueError(
            f"board {board!r} has no board-only recovery layout; name the "
            f"deployment it ships as (one of: "
            f"{', '.join(s for s in deployment_contract.available() if s.endswith('/' + board)) or 'none'})"
        )
    repository = pathlib.Path(__file__).resolve().parents[2]
    return {
        "board": board,
        "layout": BOARD_LAYOUTS[board],
        "suffix": BOARD_SUFFIXES[board],
        "partition_csv": repository / "partitions" / BOARD_PARTITION_CSV[board],
        "slot_size": None,
        "main_release_max": None,
    }


def contract_for_identity(board: str, layout: str) -> dict[str, object]:
    """Find the unique checked-in contract named by a signed manifest."""
    if board in BOARD_LAYOUTS and BOARD_LAYOUTS[board] == layout:
        return resolve_contract(board)
    for selector in deployment_contract.available():
        selected = deployment_contract.load(selector)
        if selected.board_id == board and selected.layout_id == layout:
            return resolve_contract(board, selector)
    raise ValueError(f"unknown OTA board/layout identity: {board!r} / {layout!r}")


def board_slot_size(board: str, deployment: str | None = None) -> int:
    """ota_0 capacity for a board, read from its partition CSV."""
    contract = resolve_contract(board, deployment)
    explicit = contract.get("slot_size")
    if isinstance(explicit, int):
        return explicit
    csv_path = pathlib.Path(contract["partition_csv"])
    for raw in csv_path.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        fields = [field.strip() for field in line.split(",")]
        if len(fields) >= 5 and fields[0] == "ota_0":
            return int(fields[4], 0)
    raise ValueError(f"{csv_path.name} has no ota_0 partition")


def _c_string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "strict")


def read_app_descriptor(image: pathlib.Path) -> tuple[str, str]:
    with image.open("rb") as stream:
        header = stream.read(APP_DESC_OFFSET + 256)
    if len(header) < APP_DESC_OFFSET + 256:
        raise ValueError("image is too short to contain esp_app_desc_t")
    desc = header[APP_DESC_OFFSET : APP_DESC_OFFSET + 256]
    magic = struct.unpack_from("<I", desc, 0)[0]
    if magic != APP_DESC_MAGIC:
        raise ValueError(
            f"invalid esp_app_desc_t magic 0x{magic:08x}; is this an ESP-IDF app image?"
        )
    return _c_string(desc[48:80]), _c_string(desc[16:48])


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _fixed_ascii(value: str, capacity: int, field: str) -> bytes:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as exc:
        raise ValueError(f"{field} must contain printable ASCII") from exc
    if (
        not encoded
        or len(encoded) >= capacity
        or any(byte < 0x21 or byte > 0x7E for byte in encoded)
    ):
        raise ValueError(
            f"{field} must be 1..{capacity - 1} printable non-space ASCII bytes"
        )
    return encoded + bytes(capacity - len(encoded))


def encode_payload(fields: dict[str, object]) -> bytes:
    version = semver.require(fields["version"], "version")
    minimum_updater = semver.require(
        fields["minUpdaterVersion"], "minUpdaterVersion"
    )
    payload = bytearray(PAYLOAD_SIZE)
    struct.pack_into("<IHH", payload, 0, PAYLOAD_MAGIC, FORMAT_VERSION, PAYLOAD_SIZE)
    payload[8:32] = _fixed_ascii(str(fields["boardId"]), 24, "boardId")
    payload[32:56] = _fixed_ascii(str(fields["layoutId"]), 24, "layoutId")
    payload[56:88] = _fixed_ascii(str(fields["projectName"]), 32, "projectName")
    payload[88:136] = _fixed_ascii(version, 48, "version")
    image_size = int(fields["imageSize"])
    data_schema = int(fields["dataSchema"])
    digest = bytes.fromhex(str(fields["imageSha256"]))
    if not 0 < image_size <= 0xFFFFFFFF:
        raise ValueError("imageSize is outside the unsigned 32-bit range")
    if not 0 <= data_schema <= 0xFFFFFFFF:
        raise ValueError("dataSchema is outside the unsigned 32-bit range")
    if len(digest) != 32 or not any(digest):
        raise ValueError("imageSha256 must be a nonzero 32-byte digest")
    struct.pack_into("<I", payload, 136, image_size)
    payload[140:172] = digest
    payload[172:204] = _fixed_ascii(
        minimum_updater, 32, "minUpdaterVersion"
    )
    struct.pack_into("<I", payload, 204, data_schema)
    struct.pack_into("<I", payload, 220, zlib.crc32(payload[:220]) & 0xFFFFFFFF)
    return bytes(payload)


def _read_fixed(payload: bytes, start: int, capacity: int) -> str:
    field = payload[start : start + capacity]
    end = field.find(b"\0")
    if end <= 0 or any(field[end + 1 :]):
        raise ValueError("signed payload text field is not canonically padded")
    value = field[:end]
    if any(byte < 0x21 or byte > 0x7E for byte in value):
        raise ValueError("signed payload text field contains invalid ASCII")
    return value.decode("ascii")


def decode_payload(payload: bytes) -> dict[str, object]:
    if len(payload) != PAYLOAD_SIZE:
        raise ValueError(f"signed payload must be exactly {PAYLOAD_SIZE} bytes")
    magic, version, wire_size = struct.unpack_from("<IHH", payload, 0)
    if magic != PAYLOAD_MAGIC or version != FORMAT_VERSION or wire_size != PAYLOAD_SIZE:
        raise ValueError("signed payload magic/version/size is invalid")
    if any(payload[208:220]):
        raise ValueError("signed payload reserved bytes are nonzero")
    expected_crc = zlib.crc32(payload[:220]) & 0xFFFFFFFF
    if struct.unpack_from("<I", payload, 220)[0] != expected_crc:
        raise ValueError("signed payload CRC32 is invalid")
    fields: dict[str, object] = {
        "boardId": _read_fixed(payload, 8, 24),
        "layoutId": _read_fixed(payload, 32, 24),
        "projectName": _read_fixed(payload, 56, 32),
        "version": _read_fixed(payload, 88, 48),
        "imageSize": struct.unpack_from("<I", payload, 136)[0],
        "imageSha256": payload[140:172].hex(),
        "minUpdaterVersion": _read_fixed(payload, 172, 32),
        "dataSchema": struct.unpack_from("<I", payload, 204)[0],
    }
    semver.require(fields["version"], "version")
    semver.require(fields["minUpdaterVersion"], "minUpdaterVersion")
    return fields


def run_openssl(args: list[str]) -> None:
    try:
        result = subprocess.run(
            ["openssl", *args], check=False, capture_output=True, text=True
        )
    except FileNotFoundError as exc:
        raise SystemExit("openssl is required to sign/verify OTA manifests") from exc
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip()
        suffix = f": {detail}" if detail else ""
        raise SystemExit(
            f"openssl failed with exit status {result.returncode}{suffix}"
        )


def sign_payload(payload_bytes: bytes, key: pathlib.Path) -> bytes:
    with tempfile.TemporaryDirectory(prefix="hw1-ota-manifest-") as temp_dir:
        payload_path = pathlib.Path(temp_dir) / "payload.bin"
        signature_path = pathlib.Path(temp_dir) / "payload.sig"
        payload_path.write_bytes(payload_bytes)
        run_openssl(
            [
                "dgst",
                "-sha256",
                "-sign",
                str(key),
                "-sigopt",
                "rsa_padding_mode:pss",
                "-sigopt",
                "rsa_pss_saltlen:32",
                "-out",
                str(signature_path),
                str(payload_path),
            ]
        )
        return signature_path.read_bytes()


def verify_payload(payload_bytes: bytes, signature: bytes, public_key: pathlib.Path) -> None:
    with tempfile.TemporaryDirectory(prefix="hw1-ota-verify-") as temp_dir:
        payload_path = pathlib.Path(temp_dir) / "payload.bin"
        signature_path = pathlib.Path(temp_dir) / "payload.sig"
        payload_path.write_bytes(payload_bytes)
        signature_path.write_bytes(signature)
        run_openssl(
            [
                "dgst",
                "-sha256",
                "-verify",
                str(public_key),
                "-signature",
                str(signature_path),
                "-sigopt",
                "rsa_padding_mode:pss",
                "-sigopt",
                "rsa_pss_saltlen:32",
                str(payload_path),
            ]
        )


def extract_public_key(private_key: pathlib.Path, output: pathlib.Path) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    run_openssl(
        ["pkey", "-in", str(private_key), "-pubout", "-out", str(output)]
    )


def build_envelope(fields: dict[str, object], key: pathlib.Path) -> dict[str, object]:
    """Encode and sign the canonical v1 envelope used by release/test tooling."""
    payload = encode_payload(fields)
    signature = sign_payload(payload, key)
    if len(signature) != SIGNATURE_SIZE:
        raise ValueError(
            f"unexpected signature size {len(signature)}; "
            f"expected RSA-3072 ({SIGNATURE_SIZE} bytes)"
        )
    return {
        "format": FORMAT,
        "formatVersion": FORMAT_VERSION,
        "payload": base64.b64encode(payload).decode("ascii"),
        "signature": {
            "algorithm": "rsa-pss-sha256",
            "value": base64.b64encode(signature).decode("ascii"),
        },
    }


def write_envelope(
    fields: dict[str, object], key: pathlib.Path, output: pathlib.Path
) -> None:
    envelope = build_envelope(fields, key)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(envelope, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )


def create(args: argparse.Namespace) -> None:
    image = args.image.resolve()
    if not image.is_file():
        raise SystemExit(f"image not found: {image}")
    if not args.key.is_file():
        raise SystemExit(f"private key not found: {args.key}")

    project_name, version = read_app_descriptor(image)
    if project_name != "hardwareone-idf":
        raise SystemExit(
            f"unexpected ESP project name {project_name!r}; expected 'hardwareone-idf'"
        )
    try:
        contract = resolve_contract(args.board, getattr(args, "deployment", None))
    except ValueError as exc:
        raise SystemExit(str(exc)) from exc
    expected_suffix = str(contract["suffix"])
    if not version.endswith(expected_suffix):
        raise SystemExit(
            f"image version {version!r} lacks required layout suffix {expected_suffix!r}"
        )

    image_size = image.stat().st_size
    slot_size = args.slot_size
    if slot_size is None:
        slot_size = board_slot_size(args.board, getattr(args, "deployment", None))
    if image_size <= 0 or image_size > slot_size:
        raise SystemExit(
            f"image size {image_size} does not fit OTA slot ({slot_size} bytes)"
        )
    release_limit = contract.get("main_release_max")
    if isinstance(release_limit, int) and image_size > release_limit:
        raise SystemExit(
            f"image size {image_size} exceeds deployment release gate "
            f"({release_limit} bytes)"
        )

    expected_layout = str(contract["layout"])
    if args.layout and args.layout != expected_layout:
        raise SystemExit(
            f"layout {args.layout!r} does not belong to {args.board}; "
            f"expected {expected_layout!r}"
        )
    fields: dict[str, object] = {
        "boardId": args.board,
        "dataSchema": args.data_schema,
        "imageSha256": sha256_file(image),
        "imageSize": image_size,
        "layoutId": expected_layout,
        "minUpdaterVersion": args.min_updater,
        "projectName": project_name,
        "version": version,
    }
    try:
        manifest = build_envelope(fields, args.key)
    except (ValueError, KeyError) as exc:
        raise SystemExit(f"cannot encode OTA manifest: {exc}") from exc
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    if args.public_key_out:
        extract_public_key(args.key, args.public_key_out)


def verify(args: argparse.Namespace) -> None:
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("format") != FORMAT or manifest.get("formatVersion") != FORMAT_VERSION:
        raise SystemExit("unsupported OTA manifest envelope")
    payload_value = manifest.get("payload")
    signature_info = manifest.get("signature")
    if not isinstance(payload_value, str) or not isinstance(signature_info, dict):
        raise SystemExit("manifest must contain base64 payload and signature")
    if signature_info.get("algorithm") != "rsa-pss-sha256":
        raise SystemExit("unsupported manifest signature algorithm")
    try:
        payload = base64.b64decode(payload_value, validate=True)
        signature = base64.b64decode(signature_info["value"], validate=True)
    except (KeyError, ValueError) as exc:
        raise SystemExit("manifest payload/signature is missing or invalid base64") from exc
    if len(signature) != SIGNATURE_SIZE:
        raise SystemExit("manifest signature is not an RSA-3072 signature")
    try:
        fields = decode_payload(payload)
    except ValueError as exc:
        raise SystemExit(f"invalid signed manifest payload: {exc}") from exc
    verify_payload(payload, signature, args.public_key)

    image = args.image.resolve()
    project_name, version = read_app_descriptor(image)
    checks = {
        "imageSha256": sha256_file(image),
        "imageSize": image.stat().st_size,
        "projectName": project_name,
        "version": version,
    }
    mismatches = [
        f"{key}: manifest={fields.get(key)!r}, actual={value!r}"
        for key, value in checks.items()
        if fields.get(key) != value
    ]
    if mismatches:
        raise SystemExit("manifest/image mismatch:\n  " + "\n  ".join(mismatches))
    print(
        f"OK: {fields['boardId']} {fields['layoutId']} {version} "
        f"({image.stat().st_size} bytes)"
    )


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    sub = root.add_subparsers(dest="command", required=True)

    make = sub.add_parser("create", help="create and sign a manifest")
    make.add_argument("--image", type=pathlib.Path, required=True)
    make.add_argument("--key", type=pathlib.Path, required=True)
    make.add_argument("--output", type=pathlib.Path, required=True)
    make.add_argument("--public-key-out", type=pathlib.Path)
    make.add_argument(
        "--board", choices=selectable_boards(), required=True
    )
    make.add_argument(
        "--deployment",
        choices=deployment_contract.available(),
        help="checked-in deployment contract layered onto --board",
    )
    make.add_argument(
        "--layout",
        help="optional assertion; board selects the only supported layout",
    )
    make.add_argument("--min-updater", default="1.0.0")
    make.add_argument("--data-schema", type=int, default=1)
    # Default None, resolved per board from the authoritative partition CSV.
    # A fixed 0x5A0000 default was the fifth copy of the ota_0 size in this
    # tree; it was already stale for the 16 MB layout (real slot 0x620000) and
    # is 650 KiB too LARGE for the 8 MB Feather V2 layout (real slot 0x500000),
    # so it would have signed a manifest for an image that cannot fit the slot
    # it names.
    make.add_argument("--slot-size", type=lambda value: int(value, 0), default=None)
    make.set_defaults(func=create)

    check = sub.add_parser("verify", help="verify a manifest and its image")
    check.add_argument("--manifest", type=pathlib.Path, required=True)
    check.add_argument("--image", type=pathlib.Path, required=True)
    check.add_argument("--public-key", type=pathlib.Path, required=True)
    check.set_defaults(func=verify)
    return root


def main() -> None:
    args = parser().parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
