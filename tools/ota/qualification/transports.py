"""Bounded byte transports for recovery HTTP from the host or an Android peer."""

from __future__ import annotations

import pathlib
import shutil
import socket
import subprocess
from typing import Protocol


MAX_RESPONSE_BYTES = 16 * 1024 * 1024


class ByteTransport(Protocol):
    def exchange(self, host: str, port: int, request: bytes, timeout: float) -> bytes:
        """Send one raw HTTP request and return the raw response."""


class HostSocketTransport:
    def exchange(self, host: str, port: int, request: bytes, timeout: float) -> bytes:
        if timeout <= 0:
            raise ValueError("transport timeout must be positive")
        chunks: list[bytes] = []
        total = 0
        with socket.create_connection((host, port), timeout=timeout) as connection:
            connection.settimeout(timeout)
            connection.sendall(request)
            connection.shutdown(socket.SHUT_WR)
            while True:
                try:
                    chunk = connection.recv(64 * 1024)
                except socket.timeout as exc:
                    raise RuntimeError("host recovery HTTP receive timed out") from exc
                if not chunk:
                    break
                total += len(chunk)
                if total > MAX_RESPONSE_BYTES:
                    raise RuntimeError("recovery HTTP response exceeded safety limit")
                chunks.append(chunk)
        return b"".join(chunks)


class AdbTransport:
    """Run raw HTTP from a selected Android device without exposing auth in argv."""

    def __init__(self, serial: str, adb: pathlib.Path | None = None):
        if not serial or any(character.isspace() for character in serial):
            raise ValueError("ADB serial must be explicit and contain no whitespace")
        resolved = str(adb) if adb else shutil.which("adb")
        if not resolved:
            raise RuntimeError("adb was not found; pass --adb or add it to PATH")
        self.adb = pathlib.Path(resolved).resolve()
        self.serial = serial

    def probe(self, timeout: float = 10.0) -> dict[str, str]:
        result = subprocess.run(
            [str(self.adb), "-s", self.serial, "get-state"],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        return {
            "serial": self.serial,
            "state": result.stdout.strip(),
            "stderr": result.stderr.strip(),
            "returncode": str(result.returncode),
        }

    def exchange(self, host: str, port: int, request: bytes, timeout: float) -> bytes:
        if timeout <= 0:
            raise ValueError("transport timeout must be positive")
        process = subprocess.Popen(
            [
                str(self.adb),
                "-s",
                self.serial,
                "shell",
                "-T",
                "/system/bin/nc",
                "-n",
                "-w",
                "5",
                "-W",
                str(max(1, int(timeout))),
                "-q",
                "15",
                host,
                str(port),
            ],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            stdout, stderr = process.communicate(request, timeout=timeout + 15)
        except subprocess.TimeoutExpired as exc:
            process.kill()
            stdout, stderr = process.communicate()
            if response_is_complete(stdout):
                return stdout
            raise RuntimeError("ADB recovery HTTP exchange timed out") from exc
        if process.returncode != 0 and not stdout:
            detail = stderr.decode("utf-8", "replace").strip()
            raise RuntimeError(
                f"ADB recovery HTTP transport failed ({process.returncode}): {detail}"
            )
        if len(stdout) > MAX_RESPONSE_BYTES:
            raise RuntimeError("recovery HTTP response exceeded safety limit")
        return stdout


def response_is_complete(raw: bytes) -> bool:
    marker = b"\r\n\r\n"
    if marker not in raw:
        return False
    headers, body = raw.split(marker, 1)
    for line in headers.split(b"\r\n")[1:]:
        name, separator, value = line.partition(b":")
        if separator and name.strip().lower() == b"content-length":
            try:
                return len(body) >= int(value.strip())
            except ValueError:
                return False
    return False
