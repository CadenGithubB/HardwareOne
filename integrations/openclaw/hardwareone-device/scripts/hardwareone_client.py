#!/usr/bin/env python3
"""Read-only Hardware One HTTP client. Python 3.9+, standard library only."""

import argparse
import base64
from dataclasses import dataclass, field
import http.client
import json
import math
import os
import socket
import ssl
import sys
import threading
from typing import Mapping, Optional
from urllib.parse import urlsplit


MAX_RESPONSE_BYTES = 16 * 1024
MAX_OUTPUT_BYTES = 96 * 1024
# These are the actual /api/sensors query values, not CLI sensor IDs.
SENSORS = (
    "anoencoder", "camera", "fmradio", "gps", "imu", "input",
    "microphone", "presence", "rtc", "thermal", "tof",
)
DEVICE_INFO_ROUTES = (
    ("identity", "/api/ping"),
    ("build", "/api/buildconfig"),
    ("devices", "/api/devices"),
    ("system", "/api/system"),
)


class ClientError(Exception):
    """Only fixed, credential-free messages cross the CLI boundary."""

    def __init__(self, code: str, message: str, status: Optional[int] = None):
        super().__init__(message)
        self.code = code
        self.status = status

    def result(self):
        result = {"ok": False, "error": self.code, "message": str(self)}
        if self.status is not None:
            result["http_status"] = self.status
        return result


@dataclass(frozen=True)
class Config:
    device_url: str
    username: str = field(repr=False)
    password: str = field(repr=False)
    ca_file: Optional[str] = None
    allow_http: bool = False
    timeout: float = 5.0

    @classmethod
    def from_env(cls, env: Optional[Mapping[str, str]] = None):
        env = os.environ if env is None else env
        try:
            timeout = float(env.get("HW1_TIMEOUT_SECONDS", "5"))
        except (TypeError, ValueError):
            raise ClientError("configuration", "HW1_TIMEOUT_SECONDS must be a number from 0.1 to 30.") from None
        return cls(
            device_url=env.get("HW1_DEVICE_URL", ""),
            username=env.get("HW1_USERNAME", ""),
            password=env.get("HW1_PASSWORD", ""),
            ca_file=env.get("HW1_CA_FILE") or None,
            allow_http=env.get("HW1_ALLOW_HTTP") == "1",
            timeout=timeout,
        )


class HardwareOneClient:
    def __init__(self, config: Config):
        self.config = config
        self._validate_config()
        try:
            self._ssl_context = ssl.create_default_context(cafile=config.ca_file)
        except (OSError, ssl.SSLError):
            raise ClientError("configuration", "Cannot load the configured TLS trust store.") from None
        # No requests/urllib proxy discovery: only the configured device receives auth.
        encoded = base64.b64encode(
            (config.username + ":" + config.password).encode("utf-8")
        ).decode("ascii")
        self._authorization = "Basic " + encoded

    def _validate_config(self):
        cfg = self.config
        if not cfg.device_url or any(ord(c) <= 32 or ord(c) == 127 for c in cfg.device_url):
            raise ClientError("configuration", "Set HW1_DEVICE_URL to the device's HTTPS origin.")
        try:
            target = urlsplit(cfg.device_url)
            host, port = target.hostname, target.port
            valid = (
                target.scheme in ("https", "http") and host and
                target.username is None and target.password is None and
                target.path in ("", "/") and not target.query and not target.fragment
            )
        except ValueError:
            valid = False
        if not valid:
            raise ClientError("configuration", "Device URL must be an HTTP(S) origin with no credentials, path, query, or fragment.")
        if target.scheme == "http" and not cfg.allow_http:
            raise ClientError("plaintext_disabled", "HTTP requires explicit bench opt-in with HW1_ALLOW_HTTP=1.")
        if not math.isfinite(cfg.timeout) or not 0.1 <= cfg.timeout <= 30:
            raise ClientError("configuration", "HW1_TIMEOUT_SECONDS must be a number from 0.1 to 30.")
        if (not cfg.username or not cfg.password or ":" in cfg.username or
                any(ord(c) < 32 or ord(c) == 127 for c in cfg.username + cfg.password)):
            raise ClientError("configuration", "Set valid HW1_USERNAME and HW1_PASSWORD credentials in the environment.")
        self._scheme, self._host, self._port = target.scheme, host, port

    def device_info(self):
        data = {name: self._get_json(path) for name, path in DEVICE_INFO_ROUTES}
        return {"ok": True, "operation": "device_info", "data": data}

    def read_sensor(self, sensor_id: str):
        if sensor_id not in SENSORS:
            raise ClientError("invalid_sensor", "Choose a supported sensor ID; see read_sensor --help.")
        return {
            "ok": True, "operation": "read_sensor", "sensor": sensor_id,
            "data": self._get_json("/api/sensors?sensor=" + sensor_id),
        }

    def _get_json(self, path: str):
        # Defense in depth for Python callers: this helper has no arbitrary URL API.
        if path not in {route for _, route in DEVICE_INFO_ROUTES} and path not in {
            "/api/sensors?sensor=" + sensor for sensor in SENSORS
        }:
            raise ClientError("operation_denied", "This client only supports its listed read-only operations.")
        connection = (
            http.client.HTTPSConnection(self._host, self._port, timeout=self.config.timeout,
                                        context=self._ssl_context)
            if self._scheme == "https" else
            http.client.HTTPConnection(self._host, self._port, timeout=self.config.timeout)
        )
        response = None
        timer = None
        expired = threading.Event()
        try:
            # connect() has the configured socket timeout. After connection, a
            # watchdog also bounds a trickling response (including its headers).
            connection.connect()
            wire_socket = connection.sock

            def abort_request():
                expired.set()
                try:
                    wire_socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

            timer = threading.Timer(self.config.timeout, abort_request)
            timer.daemon = True
            timer.start()
            connection.request("GET", path, headers={
                "Authorization": self._authorization,
                "Accept": "application/json",
                "Accept-Encoding": "identity",
                "Connection": "close",
            })
            response = connection.getresponse()
            if 300 <= response.status < 400:
                raise ClientError("redirect_refused", "Device redirects are not followed.", response.status)
            if response.status != 200:
                raise ClientError("http_error", "Device request was rejected.", response.status)
            if response.getheader("Content-Encoding", "identity").lower() != "identity":
                raise ClientError("invalid_response", "Compressed device responses are unsupported.")
            content_type = response.getheader("Content-Type", "").split(";", 1)[0].strip().lower()
            if content_type != "application/json" and not (
                    content_type.startswith("application/") and content_type.endswith("+json")):
                raise ClientError("invalid_response", "Device did not return JSON.")
            declared_length = response.getheader("Content-Length")
            if declared_length is not None:
                try:
                    length = int(declared_length)
                except ValueError:
                    raise ClientError("invalid_response", "Device returned an invalid content length.") from None
                if length < 0:
                    raise ClientError("invalid_response", "Device returned an invalid content length.")
                if length > MAX_RESPONSE_BYTES:
                    raise ClientError("response_too_large", "Device response exceeds the 16 KiB limit.")
            payload = response.read(MAX_RESPONSE_BYTES + 1)
            if expired.is_set():
                raise ClientError("timeout", "Device request timed out; it was not retried.")
            if len(payload) > MAX_RESPONSE_BYTES:
                raise ClientError("response_too_large", "Device response exceeds the 16 KiB limit.")
            if declared_length is not None and len(payload) != length:
                raise ClientError("invalid_response", "Device response ended before its declared length.")
            try:
                def reject_constant(_value):
                    raise ValueError()
                def finite_float(value):
                    number = float(value)
                    if not math.isfinite(number):
                        raise ValueError()
                    return number
                data = json.loads(payload.decode("utf-8"), parse_constant=reject_constant,
                                  parse_float=finite_float)
            except (UnicodeError, ValueError, RecursionError):
                raise ClientError("invalid_response", "Device returned invalid UTF-8 JSON.") from None
            if not isinstance(data, dict):
                raise ClientError("invalid_response", "Device response must be a JSON object.")
            if data.get("error") or data.get("ok") is False or data.get("success") is False:
                # Never echo a remote error body, headers, or arbitrary error text.
                safe_code = data.get("error")
                if safe_code not in ("not_compiled", "not_enabled", "data_unavailable", "auth_required"):
                    safe_code = "device_error"
                raise ClientError(safe_code, "Device reports unavailable data or denied access.")
            return data
        except ClientError:
            raise
        except ssl.SSLCertVerificationError:
            raise ClientError("tls_verification_failed", "TLS verification failed; configure a trusted device certificate.") from None
        except (TimeoutError, socket.timeout):
            raise ClientError("timeout", "Device request timed out; it was not retried.") from None
        except (OSError, http.client.HTTPException, UnicodeError, ValueError):
            if expired.is_set():
                raise ClientError("timeout", "Device request timed out; it was not retried.") from None
            raise ClientError("connection_failed", "Device connection failed; it was not retried.") from None
        finally:
            if timer is not None:
                timer.cancel()
            if response is not None:
                response.close()
            connection.close()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="operation", required=True)
    commands.add_parser("device_info", help="Read identity, compiled features, attached devices, and system state.")
    read = commands.add_parser("read_sensor", help="Read one sensor's cached data or status.")
    read.add_argument("sensor_id", choices=SENSORS)
    args = parser.parse_args(argv)
    try:
        client = HardwareOneClient(Config.from_env())
        result = client.device_info() if args.operation == "device_info" else client.read_sensor(args.sensor_id)
        try:
            output = json.dumps(result, ensure_ascii=True, separators=(",", ":"), allow_nan=False)
        except (ValueError, RecursionError):
            raise ClientError("invalid_response", "Device JSON could not be serialized safely.") from None
        if len(output.encode("ascii")) > MAX_OUTPUT_BYTES:
            raise ClientError("output_too_large", "Combined response exceeds the 96 KiB output limit.")
    except ClientError as error:
        print(json.dumps(error.result(), separators=(",", ":")))
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
