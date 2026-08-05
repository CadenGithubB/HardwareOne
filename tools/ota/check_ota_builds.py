#!/usr/bin/env python3
"""Fail closed when HardwareOne main/updater OTA artifacts have drifted.

Run this after building both projects.  It checks the generated configurations,
project identities, signed binaries, slot sizes, and the shared security/layout
contract before anything is flashed to a device.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import tempfile


APP_DESC_OFFSET = 24 + 8
APP_DESC_MAGIC = 0xABCD5432
OTA0_SIZE = 0x5A0000
UPDATER_SLOT_SIZE = 0x14E000
UPDATER_RELEASE_GATE = 0x126666  # 1.15 MiB, deliberately below slot capacity.
OTADATA_SIZE = 0x2000
EXPECTED_PARTITION_CSV = "partitions_ota_no_sr_16mb.csv"

BOARD_CONTRACT = {
    "feathers3": {
        "layout": "hw1-f3-ota-v1",
        "main_suffix": "+f3o1",
        "updater_suffix": "+f3o1",
        "flash_encryption": False,
    },
    "feathers3_fe": {
        "layout": "hw1-f3fe-ota-v1",
        "main_suffix": "+f3feo1",
        "updater_suffix": "+f3feo1",
        "flash_encryption": True,
    },
}


class Audit:
    def __init__(self) -> None:
        self.errors: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.errors.append(message)


def parse_sdkconfig(path: pathlib.Path) -> dict[str, str]:
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value.strip().strip('"')
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2 : -len(" is not set")]] = "n"
    return values


def read_description(build: pathlib.Path) -> dict[str, object]:
    path = build / "project_description.json"
    if not path.is_file():
        raise FileNotFoundError(f"missing build description: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def resolve_config(description: dict[str, object]) -> pathlib.Path:
    value = description.get("config_file")
    if not isinstance(value, str):
        raise ValueError("project description has no config_file")
    return pathlib.Path(value).resolve()


def c_string(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("utf-8", "strict")


def app_identity(path: pathlib.Path) -> tuple[str, str]:
    with path.open("rb") as stream:
        header = stream.read(APP_DESC_OFFSET + 256)
    if len(header) < APP_DESC_OFFSET + 256:
        raise ValueError(f"binary too short for app descriptor: {path}")
    descriptor = header[APP_DESC_OFFSET : APP_DESC_OFFSET + 256]
    if struct.unpack_from("<I", descriptor, 0)[0] != APP_DESC_MAGIC:
        raise ValueError(f"invalid app descriptor magic: {path}")
    return c_string(descriptor[48:80]), c_string(descriptor[16:48])


def app_binary(build: pathlib.Path, description: dict[str, object]) -> pathlib.Path:
    value = description.get("app_bin")
    if not isinstance(value, str):
        raise ValueError("project description has no app_bin")
    return build / value


def public_key_fingerprint(path: pathlib.Path) -> str:
    """Hash the canonical DER bytes, not a mutable sdkconfig pathname."""
    text = path.read_text(encoding="ascii")
    begin = "-----BEGIN PUBLIC KEY-----"
    end = "-----END PUBLIC KEY-----"
    if begin not in text or end not in text:
        raise ValueError(f"unsupported/malformed public key PEM: {path}")
    encoded = text.split(begin, 1)[1].split(end, 1)[0]
    try:
        der = base64.b64decode("".join(encoded.split()), validate=True)
    except (ValueError, binascii.Error) as exc:
        raise ValueError(f"malformed public key base64: {path}") from exc
    if not der:
        raise ValueError(f"empty public key DER: {path}")
    return hashlib.sha256(der).hexdigest()


def verify_signed_binary(public_key: pathlib.Path, image: pathlib.Path) -> str | None:
    # The generated pair-audit target invokes this script with ESP-IDF's Python
    # interpreter, but CMake/Ninja does not promise that the interpreter's
    # scripts directory is also on PATH. Invoke the installed module directly.
    result = subprocess.run(
        [
            sys.executable,
            "-m",
            "espsecure",
            "verify_signature",
            "--version",
            "2",
            "--keyfile",
            str(public_key),
            str(image),
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        return None
    detail = (result.stderr or result.stdout).strip().splitlines()
    return detail[-1] if detail else f"espsecure.py exited {result.returncode}"


def expected_partition_binary(
    description: dict[str, object], source_csv: pathlib.Path
) -> bytes:
    """Compile the authoritative CSV with the exact ESP-IDF used by the build."""
    idf_value = description.get("idf_path")
    if not isinstance(idf_value, str) or not idf_value:
        raise ValueError("project description has no idf_path")
    generator = pathlib.Path(idf_value) / "components/partition_table/gen_esp32part.py"
    if not generator.is_file():
        raise FileNotFoundError(f"missing ESP-IDF partition generator: {generator}")
    if not source_csv.is_file():
        raise FileNotFoundError(f"missing expected partition CSV: {source_csv}")
    with tempfile.TemporaryDirectory(prefix="hw1-ota-partition-") as directory:
        output = pathlib.Path(directory) / "partition-table.bin"
        result = subprocess.run(
            [
                sys.executable,
                str(generator),
                "--flash-size",
                "16MB",
                str(source_csv),
                str(output),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            detail = (result.stderr or result.stdout).strip()
            raise RuntimeError(
                f"partition generator failed ({result.returncode}): {detail}"
            )
        return output.read_bytes()


def check_value(
    audit: Audit, config: dict[str, str], key: str, expected: str, owner: str
) -> None:
    actual = config.get(key, "n")
    audit.require(actual == expected, f"{owner}: {key}={actual!r}, expected {expected!r}")


def check_common_config(audit: Audit, config: dict[str, str], owner: str) -> None:
    required = {
        "CONFIG_IDF_TARGET": "esp32s3",
        "CONFIG_ESPTOOLPY_FLASHSIZE": "16MB",
        "CONFIG_PARTITION_TABLE_OFFSET": "0x9000",
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE": "y",
        "CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK": "n",
        "CONFIG_SECURE_BOOT": "n",
        "CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT": "y",
        "CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME": "y",
        "CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT": "y",
        "CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES": "y",
        "CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP": "n",
        "CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON": "n",
        "CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS": "n",
    }
    for key, expected in required.items():
        check_value(audit, config, key, expected, owner)
    audit.require(
        bool(config.get("CONFIG_SECURE_BOOT_SIGNING_KEY")),
        f"{owner}: signing-key path is empty",
    )


def check_fe_contract(
    audit: Audit,
    main: dict[str, str],
    updater: dict[str, str],
    encrypted: bool,
) -> None:
    expected = "y" if encrypted else "n"
    for key in (
        "CONFIG_SECURE_FLASH_ENC_ENABLED",
        "CONFIG_NVS_ENCRYPTION",
        "CONFIG_NVS_SEC_KEY_PROTECT_USING_FLASH_ENC",
    ):
        check_value(audit, main, key, expected, "main")
        check_value(audit, updater, key, expected, "updater")
    if encrypted:
        for key in (
            "CONFIG_SECURE_FLASH_ENCRYPTION_MODE_DEVELOPMENT",
            "CONFIG_SECURE_FLASH_ENCRYPTION_AES128",
        ):
            check_value(audit, main, key, "y", "main")
            check_value(audit, updater, key, "y", "updater")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=sorted(BOARD_CONTRACT), required=True)
    parser.add_argument("--main-build", type=pathlib.Path, required=True)
    parser.add_argument("--updater-build", type=pathlib.Path, required=True)
    args = parser.parse_args()

    main_build = args.main_build.resolve()
    updater_build = args.updater_build.resolve()
    contract = BOARD_CONTRACT[args.board]
    audit = Audit()

    try:
        main_description = read_description(main_build)
        updater_description = read_description(updater_build)
        main_config_path = resolve_config(main_description)
        updater_config_path = resolve_config(updater_description)
        main_config = parse_sdkconfig(main_config_path)
        updater_config = parse_sdkconfig(updater_config_path)
        main_idf_revision = main_description.get("git_revision")
        updater_idf_revision = updater_description.get("git_revision")
        main_bin = app_binary(main_build, main_description)
        updater_bin = app_binary(updater_build, updater_description)
        main_public_key = main_build / "hw1_ota_public_key.pem"
        updater_public_key = updater_build / "hw1_ota_public_key.pem"
        main_partition_table = (
            main_build / "partition_table/partition-table.bin"
        ).read_bytes()
        updater_partition_table = (
            updater_build / "partition_table/partition-table.bin"
        ).read_bytes()
        repository = pathlib.Path(__file__).resolve().parents[2]
        expected_partition_table = expected_partition_binary(
            main_description, repository / EXPECTED_PARTITION_CSV
        )
        main_otadata = (main_build / "ota_data_initial.bin").read_bytes()
        updater_otadata = (updater_build / "ota_data_initial.bin").read_bytes()
        main_key_fingerprint = public_key_fingerprint(main_public_key)
        updater_key_fingerprint = public_key_fingerprint(updater_public_key)
        main_project, main_version = app_identity(main_bin)
        updater_project, updater_version = app_identity(updater_bin)
    except (OSError, RuntimeError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"OTA build audit failed: {exc}", file=sys.stderr)
        return 2

    check_common_config(audit, main_config, "main")
    check_common_config(audit, updater_config, "updater")
    check_fe_contract(
        audit,
        main_config,
        updater_config,
        bool(contract["flash_encryption"]),
    )

    audit.require(
        isinstance(main_idf_revision, str) and bool(main_idf_revision),
        "main build does not report an ESP-IDF revision",
    )
    audit.require(
        main_idf_revision == updater_idf_revision,
        f"main/updater ESP-IDF revisions differ: "
        f"{main_idf_revision!r} vs {updater_idf_revision!r}",
    )

    audit.require(
        main_partition_table == updater_partition_table,
        "main/updater emitted partition tables differ",
    )
    audit.require(
        main_partition_table == expected_partition_table,
        f"emitted partition table is not the exact {EXPECTED_PARTITION_CSV} layout",
    )
    audit.require(
        main_otadata == updater_otadata == (b"\xff" * OTADATA_SIZE),
        "main/updater OTA-data initializers are not identical blank 8 KiB images",
    )

    audit.require(
        main_key_fingerprint == updater_key_fingerprint,
        "main/updater generated public-key fingerprints differ",
    )
    main_signature_error = verify_signed_binary(main_public_key, main_bin)
    updater_signature_error = verify_signed_binary(updater_public_key, updater_bin)
    audit.require(
        main_signature_error is None,
        f"main image signature does not verify with its embedded-key source: "
        f"{main_signature_error}",
    )
    audit.require(
        updater_signature_error is None,
        f"updater image signature does not verify with its embedded-key source: "
        f"{updater_signature_error}",
    )
    audit.require(main_project == "hardwareone-idf", f"main project is {main_project!r}")
    audit.require(updater_project == "hw1-updater", f"updater project is {updater_project!r}")
    audit.require(
        main_version.endswith(str(contract["main_suffix"])),
        f"main version {main_version!r} lacks {contract['main_suffix']!r}",
    )
    audit.require(
        updater_version.endswith(str(contract["updater_suffix"])),
        f"updater version {updater_version!r} lacks {contract['updater_suffix']!r}",
    )
    audit.require(main_bin.stat().st_size <= OTA0_SIZE, "main image exceeds ota_0 slot")
    audit.require(
        updater_bin.stat().st_size <= UPDATER_SLOT_SIZE,
        "updater image exceeds factory slot",
    )
    audit.require(
        updater_bin.stat().st_size <= UPDATER_RELEASE_GATE,
        "updater image exceeds the 1.15 MiB release gate",
    )

    if audit.errors:
        print("OTA build contract FAILED:", file=sys.stderr)
        for error in audit.errors:
            print(f"  - {error}", file=sys.stderr)
        return 1

    print(
        f"OK: {args.board} / {contract['layout']} — "
        f"main {main_version} ({main_bin.stat().st_size} B), "
        f"updater {updater_version} ({updater_bin.stat().st_size} B), "
        f"key sha256:{main_key_fingerprint[:16]}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
