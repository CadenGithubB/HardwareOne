#!/usr/bin/env python3
"""Authenticated HTTP client shared by HardwareOne maintenance tools.

This module deliberately owns only the ordinary main-application web surface.
Factory-recovery HTTP has a different authentication and reboot contract and is
implemented separately by the qualification package.
"""

from __future__ import annotations

import base64
import http.cookiejar
import json
import ssl
import urllib.error
import urllib.parse
import urllib.request


FILE_RESPONSE_CONTENT_TYPE = "text/plain"
FILE_RESPONSE_CHARSET = "utf-8"
ASCII_WHITESPACE = " \t\n\r\v\f"


class DeviceHttpError(RuntimeError):
    """HTTP failure retaining structured status/body for qualification code."""

    def __init__(self, status: int, path: str, body: bytes):
        self.status = status
        self.path = path
        self.body = body
        detail = body.decode("utf-8", "replace")
        super().__init__(f"HTTP {status} for {path}: {detail}")


def normalized_login_username(username: str) -> str:
    """Match the firmware login handler's ASCII-whitespace trimming."""
    normalized = username.strip(ASCII_WHITESPACE)
    if not normalized:
        raise ValueError("username is empty after trimming")
    return normalized


class DeviceClient:
    """Authenticated client for the main firmware's maintenance endpoints."""

    def __init__(
        self,
        base_url: str,
        username: str,
        password: str,
        timeout: float,
        insecure: bool,
        *,
        user_agent: str = "hw1-ota-backup/1",
    ):
        if timeout <= 0:
            raise ValueError("timeout must be positive")
        self.base_url = base_url.rstrip("/")
        if not self.base_url:
            raise ValueError("device base URL is empty")
        self.username = normalized_login_username(username)
        self.timeout = timeout
        self.user_agent = user_agent
        cookie_jar = http.cookiejar.CookieJar()
        handlers: list[urllib.request.BaseHandler] = [
            urllib.request.HTTPCookieProcessor(cookie_jar)
        ]
        if insecure:
            context = ssl.create_default_context()
            context.check_hostname = False
            context.verify_mode = ssl.CERT_NONE
            handlers.append(urllib.request.HTTPSHandler(context=context))
        self.opener = urllib.request.build_opener(*handlers)
        login_body = urllib.parse.urlencode(
            {"username": self.username, "password": password}
        ).encode("utf-8")
        self.request(
            "/login",
            data=login_body,
            content_type="application/x-www-form-urlencoded",
        )
        if not any(cookie.name == "session" and cookie.value for cookie in cookie_jar):
            raise RuntimeError("login failed: device did not issue a session cookie")

    def request(
        self,
        path: str,
        *,
        data: bytes | None = None,
        content_type: str | None = None,
        expected_response_type: tuple[str, str] | None = None,
    ) -> bytes:
        if not path.startswith("/"):
            raise ValueError("device request path must be absolute")
        headers = {
            "Accept": "application/json",
            "User-Agent": getattr(self, "user_agent", "hw1-ota-backup/1"),
        }
        if content_type:
            headers["Content-Type"] = content_type
        request = urllib.request.Request(
            self.base_url + path,
            data=data,
            headers=headers,
            method="POST" if data is not None else "GET",
        )
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                if expected_response_type is not None:
                    expected_type, expected_charset = expected_response_type
                    actual_type = response.headers.get_content_type().lower()
                    actual_charset = response.headers.get_content_charset()
                    if actual_charset is not None:
                        actual_charset = actual_charset.lower()
                    if (
                        actual_type != expected_type
                        or actual_charset != expected_charset
                    ):
                        raise RuntimeError(
                            f"unexpected response type for {path}: "
                            f"{response.headers.get('Content-Type', '<missing>')!r}; "
                            "the device may have returned an error body"
                        )
                return response.read()
        except urllib.error.HTTPError as exc:
            raise DeviceHttpError(exc.code, path, exc.read()) from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"request failed for {path}: {exc.reason}") from exc

    def request_json(
        self,
        path: str,
        *,
        data: bytes | None = None,
        content_type: str | None = None,
    ) -> dict[str, object]:
        try:
            value = json.loads(
                self.request(path, data=data, content_type=content_type)
            )
        except json.JSONDecodeError as exc:
            raise RuntimeError(f"malformed JSON response for {path}: {exc}") from exc
        if not isinstance(value, dict):
            raise RuntimeError(f"JSON response for {path} is not an object")
        return value

    def list_dir(self, path: str) -> list[dict[str, object]]:
        query = urllib.parse.urlencode({"path": path}, quote_via=urllib.parse.quote)
        response = self.request_json(f"/api/files/list?{query}")
        if not response.get("success"):
            raise RuntimeError(f"cannot list {path}: {response.get('error', 'unknown')}")
        files = response.get("files")
        if not isinstance(files, list) or not all(
            isinstance(item, dict) for item in files
        ):
            raise RuntimeError(f"malformed listing for {path}")
        return files

    def file_stats(self, path: str = "/") -> dict[str, object]:
        query = urllib.parse.urlencode({"path": path}, quote_via=urllib.parse.quote)
        response = self.request_json(f"/api/files/stats?{query}")
        if not response.get("success"):
            raise RuntimeError(
                f"cannot read storage statistics for {path}: "
                f"{response.get('error', 'unknown')}"
            )
        for key in ("total", "used", "free", "usagePercent"):
            value = response.get(key)
            if not isinstance(value, int) or isinstance(value, bool) or value < 0:
                raise RuntimeError(f"malformed storage statistic {key!r} for {path}")
        return response

    def read_file(self, path: str) -> bytes:
        query = urllib.parse.urlencode({"name": path}, quote_via=urllib.parse.quote)
        return self.request(
            f"/api/files/read?{query}",
            expected_response_type=(
                FILE_RESPONSE_CONTENT_TYPE,
                FILE_RESPONSE_CHARSET,
            ),
        )

    def upload_file(self, path: str, content: bytes) -> None:
        body = urllib.parse.urlencode(
            {
                "path": path,
                "binary": "1",
                "content": base64.b64encode(content).decode("ascii"),
            }
        ).encode("ascii")
        query = urllib.parse.urlencode({"path": path}, quote_via=urllib.parse.quote)
        response = self.request_json(
            f"/api/files/upload?{query}",
            data=body,
            content_type="application/x-www-form-urlencoded",
        )
        if not response.get("success"):
            raise RuntimeError(
                f"upload failed for {path}: {response.get('error', 'unknown')}"
            )

    def create_dir(self, path: str) -> None:
        body = urllib.parse.urlencode({"name": path, "type": "folder"}).encode(
            "utf-8"
        )
        response = self.request_json(
            "/api/files/create",
            data=body,
            content_type="application/x-www-form-urlencoded",
        )
        if not response.get("success"):
            raise RuntimeError(
                f"directory creation failed for {path}: "
                f"{response.get('error', 'unknown')}"
            )

    def run_cli(self, command: str, *, validate: bool = False) -> str:
        if not command or "\x00" in command:
            raise ValueError("CLI command must be non-empty and contain no NUL")
        body = urllib.parse.urlencode(
            {
                "cmd": command,
                "capture": "1",
                "validate": "1" if validate else "0",
            }
        ).encode("utf-8")
        return self.request(
            "/api/cli",
            data=body,
            content_type="application/x-www-form-urlencoded",
        ).decode("utf-8", "replace")
