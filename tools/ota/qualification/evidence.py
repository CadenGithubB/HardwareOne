"""Sanitized, restartable evidence bundles for OTA hardware runs."""

from __future__ import annotations

import json
import os
import pathlib
import platform
import re
import secrets
import tempfile
from datetime import datetime, timezone
from typing import Any

from .model import Checkpoint


SENSITIVE_KEY_PARTS = (
    "password",
    "passwd",
    "authorization",
    "cookie",
    "session",
    "auth_token",
    "authtoken",
    "private_key",
    "privatekey",
    "signing_key",
    "signingkey",
)
AUTH_PATTERN = re.compile(r"(?i)\b(authorization\s*:\s*)?(basic|bearer)\s+[A-Za-z0-9+/=_-]+")
COOKIE_PATTERN = re.compile(r"(?i)\b(session|cookie)=([^;\s]+)")
EVIDENCE_FORMAT = "hardwareone-ota-qualification-evidence"
EVIDENCE_VERSION = 1


def _sensitive_key(key: object) -> bool:
    lowered = str(key).lower()
    return any(part in lowered for part in SENSITIVE_KEY_PARTS)


class Redactor:
    def __init__(self, secret_values: tuple[str, ...] = ()):
        self.secret_values = tuple(
            sorted((value for value in secret_values if value), key=len, reverse=True)
        )

    def text(self, value: str) -> str:
        result = value
        for secret in self.secret_values:
            result = result.replace(secret, "[REDACTED]")
        result = AUTH_PATTERN.sub("[REDACTED AUTH]", result)
        result = COOKIE_PATTERN.sub(r"\1=[REDACTED]", result)
        return result

    def value(self, value: Any) -> Any:
        if isinstance(value, dict):
            return {
                str(key): "[REDACTED]" if _sensitive_key(key) else self.value(item)
                for key, item in value.items()
            }
        if isinstance(value, (list, tuple)):
            return [self.value(item) for item in value]
        if isinstance(value, str):
            return self.text(value)
        if isinstance(value, (int, float, bool)) or value is None:
            return value
        return self.text(str(value))


def atomic_json(path: pathlib.Path, value: object, redactor: Redactor) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    sanitized = redactor.value(value)
    payload = json.dumps(sanitized, indent=2, sort_keys=True) + "\n"
    descriptor, temporary_name = tempfile.mkstemp(
        prefix=path.name + ".", suffix=".tmp", dir=path.parent
    )
    temporary = pathlib.Path(temporary_name)
    try:
        os.fchmod(descriptor, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            stream.write(payload)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def default_output_root() -> pathlib.Path:
    return pathlib.Path(tempfile.gettempdir()) / "hw1-ota-qualification"


def new_run_id() -> str:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return f"{stamp}-{secrets.token_hex(4)}"


class EvidenceRecorder:
    def __init__(
        self,
        root: pathlib.Path,
        *,
        run_id: str | None = None,
        secret_values: tuple[str, ...] = (),
    ):
        self.root = root.resolve()
        self.run_id = run_id or new_run_id()
        if not re.fullmatch(r"[A-Za-z0-9._-]{8,96}", self.run_id):
            raise ValueError("run ID contains unsafe characters")
        self.directory = self.root / self.run_id
        self.redactor = Redactor(secret_values)

    def create(self) -> pathlib.Path:
        root_existed = self.root.exists()
        self.root.mkdir(parents=True, exist_ok=True)
        if not self.root.is_dir():
            raise ValueError("evidence root is not a directory")
        if not root_existed:
            os.chmod(self.root, 0o700)
        self.directory.mkdir(mode=0o700)
        self.write_json(
            "environment.json",
            {
                "format": EVIDENCE_FORMAT,
                "formatVersion": EVIDENCE_VERSION,
                "runId": self.run_id,
                "createdAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "host": {
                    "platform": platform.platform(),
                    "python": platform.python_version(),
                },
            },
        )
        return self.directory

    def write_json(self, relative: str, value: object) -> None:
        path = self.directory / relative
        try:
            path.resolve().relative_to(self.directory)
        except ValueError as exc:
            raise ValueError("evidence path escapes the run directory") from exc
        atomic_json(path, value, self.redactor)

    def append_event(self, kind: str, fields: dict[str, object]) -> None:
        if not re.fullmatch(r"[a-z0-9_.-]{1,64}", kind):
            raise ValueError("invalid evidence event kind")
        event = self.redactor.value(
            {
                "at": datetime.now(timezone.utc).isoformat(timespec="milliseconds"),
                "kind": kind,
                **fields,
            }
        )
        path = self.directory / "events.jsonl"
        flags = os.O_WRONLY | os.O_CREAT | os.O_APPEND
        descriptor = os.open(path, flags, 0o600)
        with os.fdopen(descriptor, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(event, sort_keys=True) + "\n")
            stream.flush()
            os.fsync(stream.fileno())

    def write_checkpoint(self, checkpoint: Checkpoint) -> None:
        if checkpoint.run_id != self.run_id:
            raise ValueError("checkpoint run ID does not match evidence directory")
        self.write_json("checkpoint.json", checkpoint.as_dict())

    @staticmethod
    def load_checkpoint(run_directory: pathlib.Path) -> Checkpoint:
        value = json.loads(
            (run_directory.resolve() / "checkpoint.json").read_text(encoding="utf-8")
        )
        return Checkpoint.from_dict(value)
