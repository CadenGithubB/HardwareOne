"""Small raw HTTP/1.1 client for the factory recovery updater."""

from __future__ import annotations

import base64
import dataclasses
import json

from .transports import ByteTransport


class NoHttpResponse(RuntimeError):
    """The updater rebooted or closed without returning a status line."""


@dataclasses.dataclass(frozen=True)
class RecoveryResponse:
    status: int
    headers: dict[str, str]
    body: bytes

    def json(self) -> dict[str, object]:
        try:
            value = json.loads(self.body)
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"recovery returned malformed JSON: {exc}") from exc
        if not isinstance(value, dict):
            raise RuntimeError("recovery JSON response is not an object")
        return value


def parse_http(raw: bytes) -> RecoveryResponse:
    if not raw:
        raise NoHttpResponse("peer closed without an HTTP response")
    marker = b"\r\n\r\n"
    if marker not in raw:
        raise RuntimeError(f"malformed recovery HTTP response ({len(raw)} bytes)")
    header_block, body = raw.split(marker, 1)
    lines = header_block.split(b"\r\n")
    fields = lines[0].split()
    if len(fields) < 2 or not fields[1].isdigit():
        raise RuntimeError(f"malformed recovery HTTP status line: {lines[0]!r}")
    headers: dict[str, str] = {}
    for line in lines[1:]:
        name, separator, value = line.partition(b":")
        if not separator:
            raise RuntimeError(f"malformed recovery HTTP header: {line!r}")
        headers[name.decode("ascii", "strict").lower()] = value.decode(
            "latin-1"
        ).strip()
    content_length = headers.get("content-length")
    if content_length is not None:
        try:
            expected = int(content_length)
        except ValueError as exc:
            raise RuntimeError("invalid recovery Content-Length") from exc
        if len(body) < expected:
            raise RuntimeError(
                f"incomplete recovery HTTP body: expected {expected}, got {len(body)}"
            )
        body = body[:expected]
    return RecoveryResponse(int(fields[1]), headers, body)


class RecoveryClient:
    def __init__(
        self,
        transport: ByteTransport,
        credential: str,
        *,
        host: str = "192.168.77.1",
        port: int = 80,
        timeout: float = 45.0,
    ):
        if not credential:
            raise ValueError("recovery credential is empty")
        self.transport = transport
        self.credential = credential
        self.host = host
        self.port = port
        self.timeout = timeout

    def request(
        self,
        method: str,
        path: str,
        body: bytes = b"",
        *,
        content_type: str | None = None,
        timeout: float | None = None,
    ) -> RecoveryResponse:
        if method not in {"GET", "POST", "PUT"} or not path.startswith("/"):
            raise ValueError("invalid recovery HTTP method/path")
        encoded = base64.b64encode(
            ("admin:" + self.credential).encode("utf-8")
        ).decode("ascii")
        headers = [
            f"{method} {path} HTTP/1.1",
            f"Host: {self.host}",
            f"Authorization: Basic {encoded}",
            f"Content-Length: {len(body)}",
            "Connection: close",
        ]
        if content_type:
            headers.append(f"Content-Type: {content_type}")
        wire = ("\r\n".join(headers) + "\r\n\r\n").encode("ascii") + body
        raw = self.transport.exchange(
            self.host, self.port, wire, timeout if timeout is not None else self.timeout
        )
        return parse_http(raw)

    def status(self) -> dict[str, object]:
        response = self.request("GET", "/status")
        if response.status != 200:
            raise RuntimeError(f"recovery status returned HTTP {response.status}")
        return response.json()

    def post_manifest(self, manifest: bytes) -> RecoveryResponse:
        return self.request(
            "POST", "/manifest", manifest, content_type="application/json"
        )

    def put_firmware(self, image: bytes) -> RecoveryResponse:
        return self.request(
            "PUT",
            "/firmware",
            image,
            content_type="application/octet-stream",
            timeout=600.0,
        )

    def action(self, name: str) -> RecoveryResponse:
        if name not in {"apply", "cancel", "allow-downgrade", "reboot"}:
            raise ValueError("unsupported recovery action")
        return self.request("POST", "/" + name)
