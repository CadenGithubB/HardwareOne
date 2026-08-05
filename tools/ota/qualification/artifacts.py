"""Read-only artifact identity and paired-build qualification checks."""

from __future__ import annotations

import base64
import dataclasses
import hashlib
import json
import pathlib
import subprocess
import sys

from tools.ota import make_manifest


@dataclasses.dataclass(frozen=True)
class ArtifactIdentity:
    board: str
    layout: str
    project: str
    version: str
    image_size: int
    image_sha256: str
    manifest_sha256: str
    minimum_updater: str
    data_schema: int

    def as_dict(self) -> dict[str, object]:
        return dataclasses.asdict(self)


def sha256(path: pathlib.Path) -> str:
    return make_manifest.sha256_file(path)


def load_verified_artifacts(
    image: pathlib.Path,
    manifest_path: pathlib.Path,
    public_key: pathlib.Path,
    *,
    expected_board: str,
) -> ArtifactIdentity:
    image = image.resolve()
    manifest_path = manifest_path.resolve()
    public_key = public_key.resolve()
    for label, path in (
        ("image", image),
        ("manifest", manifest_path),
        ("public key", public_key),
    ):
        if not path.is_file():
            raise ValueError(f"{label} not found: {path}")

    try:
        envelope = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"manifest is not valid UTF-8 JSON: {exc}") from exc
    if not isinstance(envelope, dict) or set(envelope) != {
        "format",
        "formatVersion",
        "payload",
        "signature",
    }:
        raise ValueError("manifest envelope does not have the exact v1 key set")
    if (
        envelope.get("format") != make_manifest.FORMAT
        or envelope.get("formatVersion") != make_manifest.FORMAT_VERSION
    ):
        raise ValueError("unsupported OTA manifest envelope")
    signature_info = envelope.get("signature")
    if not isinstance(signature_info, dict) or set(signature_info) != {
        "algorithm",
        "value",
    }:
        raise ValueError("manifest signature object is malformed")
    if signature_info.get("algorithm") != "rsa-pss-sha256":
        raise ValueError("unsupported manifest signature algorithm")
    payload_value = envelope.get("payload")
    signature_value = signature_info.get("value")
    if not isinstance(payload_value, str) or not isinstance(signature_value, str):
        raise ValueError("manifest payload/signature is not base64 text")
    try:
        payload = base64.b64decode(payload_value, validate=True)
        signature = base64.b64decode(signature_value, validate=True)
    except ValueError as exc:
        raise ValueError("manifest payload/signature base64 is invalid") from exc
    if len(signature) != make_manifest.SIGNATURE_SIZE:
        raise ValueError("manifest signature is not RSA-3072")
    fields = make_manifest.decode_payload(payload)
    make_manifest.verify_payload(payload, signature, public_key)

    project, version = make_manifest.read_app_descriptor(image)
    comparisons = {
        "boardId": expected_board,
        "layoutId": make_manifest.BOARD_LAYOUTS[expected_board],
        "projectName": project,
        "version": version,
        "imageSize": image.stat().st_size,
        "imageSha256": sha256(image),
    }
    mismatches = [
        f"{key}: manifest={fields.get(key)!r}, expected={expected!r}"
        for key, expected in comparisons.items()
        if fields.get(key) != expected
    ]
    if mismatches:
        raise ValueError("artifact identity mismatch: " + "; ".join(mismatches))
    return ArtifactIdentity(
        board=str(fields["boardId"]),
        layout=str(fields["layoutId"]),
        project=project,
        version=version,
        image_size=image.stat().st_size,
        image_sha256=str(fields["imageSha256"]),
        manifest_sha256=sha256(manifest_path),
        minimum_updater=str(fields["minUpdaterVersion"]),
        data_schema=int(fields["dataSchema"]),
    )


@dataclasses.dataclass(frozen=True)
class PairAuditResult:
    passed: bool
    command: tuple[str, ...]
    output: str
    returncode: int

    def as_dict(self) -> dict[str, object]:
        return {
            "passed": self.passed,
            "command": list(self.command),
            "output": self.output,
            "returncode": self.returncode,
        }


def run_pair_audit(
    board: str,
    main_build: pathlib.Path,
    updater_build: pathlib.Path,
) -> PairAuditResult:
    script = pathlib.Path(__file__).resolve().parents[1] / "check_ota_builds.py"
    command = (
        sys.executable,
        str(script),
        "--board",
        board,
        "--main-build",
        str(main_build.resolve()),
        "--updater-build",
        str(updater_build.resolve()),
    )
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    output = "\n".join(
        part.strip() for part in (result.stdout, result.stderr) if part.strip()
    )
    return PairAuditResult(result.returncode == 0, command, output, result.returncode)


def file_metadata(path: pathlib.Path) -> dict[str, object]:
    path = path.resolve()
    return {"name": path.name, "size": path.stat().st_size, "sha256": sha256(path)}


def public_key_fingerprint(path: pathlib.Path) -> str:
    text = path.read_text(encoding="ascii")
    begin = "-----BEGIN PUBLIC KEY-----"
    end = "-----END PUBLIC KEY-----"
    if begin not in text or end not in text:
        raise ValueError("public key is not a canonical PUBLIC KEY PEM")
    der = base64.b64decode(
        "".join(text.split(begin, 1)[1].split(end, 1)[0].split()), validate=True
    )
    return hashlib.sha256(der).hexdigest()
