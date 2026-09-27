#!/usr/bin/env python3
"""HardwareOne HTTP checks through an already-open S3 USB-admin console.

Importing this file never opens ports or performs network operations. The caller
owns a ConnectivityConsole (hardwareone completion barrier), login and its
redaction policy. Literal credential/base64 redaction alone is insufficient:
HTTP response bodies embed credentials inside a larger base64 value, so the
connectivity subclass omits those body fields from logs while preserving raw
in-memory replies for verification. This helper never returns/logs response bodies, form data or
cookies; its private result contains only request metadata and assertions.
"""
from __future__ import annotations

import base64
from dataclasses import dataclass, field
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import time
import urllib.parse

HERE = Path(__file__).resolve().parent


class HttpBridgeError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise HttpBridgeError(message)


def private_encodings(credentials):
    """Same redaction additions the owning serial coordinator must install."""
    values = {value for value in credentials.values() if isinstance(value, str) and value}
    values.update(base64.b64encode(value.encode()).decode() for value in tuple(values))
    form = urllib.parse.urlencode({key: credentials[key] for key in ("username", "password")})
    values.update((form, base64.b64encode(form.encode()).decode()))
    return sorted(values, key=len, reverse=True)


def parse_bridge(output):
    decoder = json.JSONDecoder()
    for index, char in enumerate(output):
        if char != "{":
            continue
        try:
            value, _ = decoder.raw_decode(output[index:])
        except ValueError:
            continue
        if not isinstance(value, dict) or value.get("schema") != 1:
            continue
        if value.get("bridge") != "s3-http-v1" and "error" not in value:
            continue
        if value.get("ok") is not True:
            reason = value.get("error", "unknown")
            # Fixture error identifiers are fixed tokens, never reflected data.
            if not isinstance(reason, str) or re.fullmatch(r"[a-z_]{1,80}", reason) is None:
                reason = "invalid_error_response"
            raise HttpBridgeError("S3 HTTP bridge failed: " + reason)
        require(value.get("bridge") == "s3-http-v1", "Wrong serial HTTP bridge")
        return value
    raise HttpBridgeError("No complete S3 HTTP bridge JSON response")


@dataclass
class Reply:
    status: int
    location: str
    metadata: dict
    body: bytes | None = field(repr=False)

    def text(self):
        require(self.body is not None, "Required response body was too large for USB JSON transport")
        try:
            return self.body.decode("utf-8", errors="strict")
        except UnicodeDecodeError:
            raise HttpBridgeError("HTTP body was not valid UTF-8") from None


def validate_reply(document):
    require(document.get("complete") is True, "HTTP response was incomplete")
    require(type(document.get("status")) is int and 100 <= document["status"] <= 599, "Invalid HTTP status")
    require(type(document.get("bytes")) is int and 0 <= document["bytes"] <= 2_000_000, "Invalid HTTP response byte count")
    require(isinstance(document.get("sha256"), str) and re.fullmatch(r"[0-9a-f]{64}", document["sha256"]), "Invalid HTTP response hash")
    require(isinstance(document.get("location"), str), "Invalid HTTP redirect location")
    require(type(document.get("bodyIncluded")) is bool, "Missing HTTP body inclusion flag")
    body = None
    if document["bodyIncluded"]:
        require(document["bytes"] <= 8192 and isinstance(document.get("body"), str), "Invalid included HTTP body")
        try:
            body = base64.b64decode(document["body"], validate=True)
        except (ValueError, TypeError):
            raise HttpBridgeError("Invalid base64 HTTP body") from None
        require(len(body) == document["bytes"] and hashlib.sha256(body).hexdigest() == document["sha256"],
                "HTTP body length/hash differs from S3 summary")
    else:
        require(document["bytes"] > 8192 and "body" not in document, "Unexpected omission of a small HTTP body")
    # Allowlist fields: in particular never persist body/base64 or future cookie
    # header fields accidentally added by the fixture.
    metadata = {key: document[key] for key in ("status", "bytes", "sha256", "complete", "durationMs",
                "htmlOpen", "htmlClose", "hasUsername", "hasPassword", "cookiePresent", "bodyIncluded") if key in document}
    return Reply(document["status"], document["location"], metadata, body)


def require_denied(reply):
    require(reply.status in (401, 403) or (reply.status in (302, 303) and reply.location.startswith("/login")),
            "Protected API did not reject an unauthenticated request")


def command_json(body):
    begin = body.find("{")
    require(begin >= 0, "Missing HTTP command JSON")
    try:
        value, _ = json.JSONDecoder().raw_decode(body[begin:])
    except ValueError:
        raise HttpBridgeError("Incomplete HTTP command JSON") from None
    require(isinstance(value, dict) and "error" not in value, "Invalid HTTP command JSON")
    return value


class HttpS3Probe:
    def __init__(self, board, credentials, *, timeout=45):
        self.board, self.credentials, self.timeout = board, credentials, timeout
        self.evidence = []
        self.last_command_started = 0.0

    def rpc(self, arguments):
        command = "httpprobe " + arguments
        require(len(command.encode()) <= 2047, "HTTP bridge command exceeds serial input bound")
        return parse_bridge(self.board.command(command, timeout=self.timeout))

    def join(self):
        ssid = self.credentials.get("ap_ssid", "HW1_P4_TEST")
        password = self.credentials["ap_password"]
        require(isinstance(ssid, str) and 1 <= len(ssid.encode()) <= 32, "Invalid private test SSID length")
        require(isinstance(password, str) and 8 <= len(password.encode()) <= 63, "Invalid private test AP password length")
        encode = lambda value: base64.b64encode(value.encode()).decode()
        result = self.rpc("join " + encode(ssid) + " " + encode(password))
        self.validate_network(result)
        return {key: result[key] for key in ("ip", "apIp", "channel", "connected")}

    def validate_network(self, result):
        require(result.get("connected") is True and result.get("channel") == 6, "S3 is not connected on test channel 6")
        require(str(result.get("ip", "")).startswith("192.168.4.") and result.get("apIp") == "192.168.5.1",
                "S3 test network routing does not match the isolated fixture")

    def request(self, path, fields=None):
        require(path in ("/login", "/api/system", "/dashboard", "/settings", "/bluetooth", "/espnow", "/cli",
                         "/api/buildconfig", "/api/cli", "/logout"), "HTTP path is outside the test allowlist")
        method = "POST" if fields is not None else "GET"
        arguments = f"request {method} {path}"
        if fields is not None:
            raw = urllib.parse.urlencode(fields).encode()
            require(0 < len(raw) <= 1400, "HTTP form exceeds the fixture bound")
            arguments += " " + base64.b64encode(raw).decode()
        document = self.rpc(arguments)
        require(document.get("connected") is True, "S3 Wi-Fi disconnected during HTTP test")
        reply = validate_reply(document)
        self.evidence.append({"path": path, "method": method, **reply.metadata})
        return reply

    def command(self, command):
        require(command in ("whoami", "status json", "blestatus json", "espnowstatus json"), "Command outside HTTP probe allowlist")
        time.sleep(max(0.0, 0.075 - (time.monotonic() - self.last_command_started)))
        self.last_command_started = time.monotonic()
        reply = self.request("/api/cli", {"cmd": command, "capture": "1"})
        body = reply.text()
        require(reply.status == 200 and "Unknown command" not in body and not body.startswith(("ERROR:", "Error:")),
                "Authenticated HTTP command failed: " + command)
        return body

    def verify(self, duration=0, on_cycle=None):
        cleared = self.rpc("clear")
        require(cleared.get("cookiePresent") is False, "HTTP fixture did not clear its previous cookie")
        reply = self.request("/login")
        require(reply.status == 200 and reply.metadata.get("hasUsername") is True and reply.metadata.get("hasPassword") is True,
                "Login form unavailable")
        require_denied(self.request("/api/system"))
        reply = self.request("/login", {key: self.credentials[key] for key in ("username", "password")})
        require(reply.status == 303 and reply.location == "/dashboard" and reply.metadata.get("cookiePresent") is True,
                "Named HTTP cookie login failed")
        for path in ("/dashboard", "/settings", "/bluetooth", "/espnow", "/cli"):
            reply = self.request(path)
            require(reply.status == 200 and reply.metadata.get("htmlOpen") is True and reply.metadata.get("htmlClose") is True,
                    "Incomplete HTML page: " + path)
        for path in ("/api/system", "/api/buildconfig"):
            reply = self.request(path)
            try:
                value = json.loads(reply.text())
            except ValueError:
                raise HttpBridgeError("Invalid JSON API: " + path) from None
            require(reply.status == 200 and isinstance(value, dict) and "error" not in value, "Invalid JSON API: " + path)
        identity = self.command("whoami")
        require(self.credentials["username"] in identity and "admin" in identity, "Wrong HTTP command identity")
        summaries = {}
        for command in ("status json", "blestatus json", "espnowstatus json"):
            document = command_json(self.command(command))
            summaries[command] = {"completeJson": True, "schema": document.get("schema")}
        deadline, cycles, coexistence = time.monotonic() + duration, 0, []
        while time.monotonic() < deadline or (cycles == 0 and on_cycle is not None):
            for path in ("/api/system", "/bluetooth"):
                reply = self.request(path)
                require(reply.status == 200 and reply.metadata["bytes"] > 0, "Concurrent HTTP read failed")
            if on_cycle is not None:
                # Callback return data may contain arbitrary subsystem payloads;
                # keep it with the caller and record only that it succeeded.
                on_cycle(cycles)
                coexistence.append({"cycle": cycles, "completed": True})
                reply = self.request("/api/system")
                require(reply.status == 200 and reply.metadata["bytes"] > 0, "HTTP failed after coexistence callback")
            cycles += 1
            if time.monotonic() < deadline:
                time.sleep(0.4)
        self.request("/logout")
        require_denied(self.request("/api/system"))
        return {"cookieLogin": True, "protectedApi": True, "namedAdminCli": True,
                "htmlPages": 5, "jsonApis": 2, "commandJson": summaries,
                "logout": True, "loadCycles": cycles, "coexistenceCallbacks": coexistence}


def run_probe_http(board, credentials, duration=0, join=True, *, on_cycle=None, run_root=None):
    """Synchronous API; caller owns its authenticated ConnectivityConsole.

    duration holds the authenticated session for periodic reads (0..600 seconds).
    Optional on_cycle(index) performs synchronous mesh/BLE work before another
    HTTP read. With duration=0 it runs once; it must raise on failure. The cookie
    stays private inside S3 RAM and is logged out at completion. No port closes.
    """
    require(isinstance(credentials, dict), "Credentials must be a private JSON object")
    for key in ("username", "password", "ap_password"):
        require(isinstance(credentials.get(key), str) and credentials[key], "Missing private credential field " + key)
    require(isinstance(duration, (int, float)) and 0 <= duration <= 600, "Duration must be between 0 and 600 seconds")
    private_values = private_encodings(credentials)
    def safe(value):
        if isinstance(value, str):
            for secret in private_values:
                value = value.replace(secret, "[REDACTED]")
            return value
        if isinstance(value, list):
            return [safe(item) for item in value]
        if isinstance(value, dict):
            return {safe(str(key)): safe(item) for key, item in value.items()}
        return value
    root = Path(run_root) if run_root else HERE / "private" / "http-s3-runs"
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    directory = root / f"{stamp}-{secrets.token_hex(4)}"
    directory.mkdir(mode=0o700)
    destination = directory / "results.json"
    result = {"schema": 1, "status": "running", "started": stamp,
              "transport": "S3 Wi-Fi HTTP client through caller-owned USB serial", "url": "http://192.168.4.1",
              "durationSec": duration, "resultPath": str(destination)}
    probe = HttpS3Probe(board, credentials)
    def save():
        result["requests"] = probe.evidence
        descriptor = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            json.dump(safe(result), output, indent=2)
            output.write("\n")
    try:
        save()
        identity = board.command("whoami", timeout=45)
        require(f"You are {credentials['username']} (admin)" in identity, "S3 console is not authenticated as the expected admin")
        result["network"] = probe.join() if join else probe.rpc("status")
        probe.validate_network(result["network"])
        result["verified"] = probe.verify(duration, on_cycle)
        result.update(status="passed", finished=dt.datetime.now(dt.timezone.utc).isoformat())
        save()
        return safe(result)
    except BaseException as error:
        result.update(status="failed", error=str(error), finished=dt.datetime.now(dt.timezone.utc).isoformat())
        # Remove cookie state on a failing run without masking its first error.
        try:
            probe.rpc("clear")
            result["failureCookieCleared"] = True
        except BaseException:
            result["failureCookieCleared"] = False
        try:
            save()
        except BaseException:
            pass
        raise HttpBridgeError(safe(str(error))) from None


if __name__ == "__main__":
    import argparse
    argparse.ArgumentParser(description=__doc__ + "\nUse run_probe_http() from board_control; this file does not open ports.").parse_args()
