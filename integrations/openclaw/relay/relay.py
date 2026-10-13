#!/usr/bin/env python3
"""A bounded TLS chat relay for one Hardware One device and local OpenClaw main.

This restricts the HTTP surface and keeps the gateway credential off the device.
It does not restrict agent main's allowed tools or make its user prompts harmless.
No request is retried: after a disconnect, agent actions may already have run.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, fields
import hmac
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import socketserver
import ssl
import stat
import sys
import threading
import time

HEADER_TIMEOUT = 5.0
BODY_TIMEOUT = 5.0
IO_TIMEOUT = 30.0
WRITE_TIMEOUT = 5.0
TOTAL_TIMEOUT = 300.0
MAX_HEADERS = 8192
MAX_BODY = 14336
MAX_EVENT = 8192
MAX_REPLY = 1024 * 1024
MAX_CLIENTS = 4
CHAT_PATH = "/v1/chat/completions"
AGENT_MODEL = "openclaw/main"
ALLOWED_HEADERS = frozenset(("host", "content-type", "accept", "user-agent",
                             "connection", "content-length", "authorization"))


class ConfigError(ValueError):
    pass


class RequestError(ValueError):
    def __init__(self, message="Invalid request", status=400):
        super().__init__(message)
        self.status = status


class UpstreamError(ValueError):
    pass


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key")
        result[key] = value
    return result


def _json(raw):
    def invalid_constant(_):
        raise ValueError("Non-finite JSON number")
    return json.loads(raw.decode("utf-8", errors="strict"),
                      object_pairs_hook=_unique_object, parse_constant=invalid_constant)


@dataclass(frozen=True)
class Config:
    bind_host: str
    bind_port: int
    device_identity: str
    tls_cert_file: str
    tls_key_file: str
    device_token_file: str
    upstream_token_file: str
    upstream_port: int = 18789

    def __post_init__(self):
        if not isinstance(self.bind_host, str):
            raise ConfigError("bind_host must be an explicit IPv4 address")
        try:
            address = ipaddress.IPv4Address(self.bind_host)
        except (ValueError, TypeError):
            raise ConfigError("bind_host must be an explicit IPv4 address") from None
        if address.is_unspecified or address.is_multicast or address.is_reserved:
            raise ConfigError("bind_host must name a specific local interface")
        for port in (self.bind_port, self.upstream_port):
            if type(port) is not int or not 1 <= port <= 65535:
                raise ConfigError("Configured ports must be between 1 and 65535")
        if not isinstance(self.device_identity, str) or not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,31}", self.device_identity):
            raise ConfigError("device_identity must be a short lowercase identifier")
        for name in ("tls_cert_file", "tls_key_file", "device_token_file", "upstream_token_file"):
            value = getattr(self, name)
            if not isinstance(value, str) or not value or not Path(value).is_absolute() or "\x00" in value:
                raise ConfigError("Certificate and credential paths must be absolute")


def _private_file(path, maximum):
    """Read a small owner-only regular file, refusing symlinks and loose modes."""
    descriptor = None
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | os.O_NONBLOCK)
        info = os.fstat(descriptor)
        if (not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or
                info.st_uid != os.getuid() or info.st_size > maximum):
            raise ConfigError("Credential files must be owned by this user, regular, and mode 0600 or stricter")
        with os.fdopen(descriptor, "rb") as source:
            descriptor = None
            data = source.read(maximum + 1)
        if len(data) > maximum:
            raise ConfigError("Credential file exceeds its size limit")
        return data
    except OSError:
        raise ConfigError("Cannot read a private credential file") from None
    finally:
        if descriptor is not None:
            os.close(descriptor)


def _token(path, minimum):
    data = _private_file(path, 1026).rstrip(b"\r\n")
    if not minimum <= len(data) <= 1024 or any(c < 33 or c > 126 for c in data):
        raise ConfigError("Tokens must be printable ASCII without spaces; device token needs 32 characters")
    return data.decode("ascii")


def _credentials(config):
    device, upstream = _token(config.device_token_file, 32), _token(config.upstream_token_file, 1)
    if hmac.compare_digest(device, upstream):
        raise ConfigError("Device and upstream credentials must be distinct")
    return device, upstream


def create_tls_context(config):
    _private_file(config.tls_key_file, 16384)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    try:
        # An encrypted/misconfigured key must fail unattended preflight instead
        # of letting OpenSSL open an interactive password prompt.
        context.load_cert_chain(config.tls_cert_file, config.tls_key_file, password=lambda: "")
    except (OSError, ssl.SSLError):
        raise ConfigError("Cannot load the configured TLS certificate and private key") from None
    return context


def load_config(path, check_files=True):
    try:
        with open(path, "rb") as source:
            raw = source.read(16385)
        if len(raw) > 16384:
            raise ValueError("Configuration too large")
        data = _json(raw)
        if not isinstance(data, dict) or set(data) - {field.name for field in fields(Config)}:
            raise ValueError("Unknown configuration fields")
        config = Config(**data)
    except (OSError, ValueError, TypeError, RecursionError):
        raise ConfigError("Invalid relay configuration; check its fields and absolute paths") from None
    if check_files:
        _credentials(config)
        create_tls_context(config)
    return config


def validate_request(raw, device_identity):
    """Return a newly constructed upstream body, never the untrusted input object."""
    if not isinstance(raw, bytes) or not 0 < len(raw) <= MAX_BODY:
        raise RequestError()
    try:
        value = _json(raw)
        if not isinstance(value, dict) or set(value) != {"model", "user", "stream", "messages"}:
            raise ValueError()
        if value["model"] != AGENT_MODEL or value["stream"] is not True:
            raise ValueError()
        conversation = value["user"]
        if not isinstance(conversation, str) or not re.fullmatch(r"hw1:[0-9a-f]{32}", conversation):
            raise ValueError()
        messages = value["messages"]
        if not isinstance(messages, list) or len(messages) != 1:
            raise ValueError()
        message = messages[0]
        if not isinstance(message, dict) or set(message) != {"role", "content"} or message["role"] != "user":
            raise ValueError()
        content = message["content"]
        if not isinstance(content, str) or not content or len(content.encode("utf-8")) > 2047 or "\x00" in content:
            raise ValueError()
        upstream = {"model": AGENT_MODEL, "user": "hw1:" + device_identity + ":" + conversation[4:],
                    "stream": True, "messages": [{"role": "user", "content": content}]}
        return json.dumps(upstream, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    except (ValueError, TypeError, UnicodeError, RecursionError):
        raise RequestError() from None


def _remaining(deadline, maximum):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError()
    return min(remaining, maximum)


def _receive_request(connection, expected_token):
    data = bytearray()
    deadline = time.monotonic() + HEADER_TIMEOUT
    while b"\r\n\r\n" not in data:
        connection.settimeout(_remaining(deadline, HEADER_TIMEOUT))
        chunk = connection.recv(min(4096, MAX_HEADERS + 4 - len(data)))
        if not chunk:
            raise RequestError()
        data.extend(chunk)
        boundary = data.find(b"\r\n\r\n")
        if (boundary < 0 and len(data) > MAX_HEADERS) or boundary > MAX_HEADERS:
            raise RequestError(status=431)
    raw_headers, body = bytes(data).split(b"\r\n\r\n", 1)
    try:
        lines = raw_headers.decode("ascii", errors="strict").split("\r\n")
        match = re.fullmatch(r"([A-Z]+) ([^ ]+) HTTP/1\.[01]", lines[0])
        if not match or len(lines) > 33:
            raise ValueError()
        method, path = match.groups()
        headers = {}
        for line in lines[1:]:
            name, value = line.split(":", 1)
            name = name.lower()
            if not re.fullmatch(r"[a-z0-9-]+", name) or name not in ALLOWED_HEADERS or name in headers:
                raise ValueError()
            if any(ord(c) < 32 or ord(c) == 127 for c in value):
                raise ValueError()
            headers[name] = value.strip(" ")
        if not headers.get("host"):
            raise ValueError()
        authorization = headers.get("authorization", "")
        if not hmac.compare_digest(authorization, "Bearer " + expected_token):
            raise RequestError("Unauthorized", 401)
        if method != "POST":
            raise RequestError("Method not allowed", 405)
        if path != CHAT_PATH:
            raise RequestError("Not found", 404)
        if headers.get("content-type", "").split(";", 1)[0].strip().lower() != "application/json":
            raise RequestError("Unsupported media type", 415)
        length_text = headers.get("content-length", "")
        if not re.fullmatch(r"[0-9]{1,6}", length_text):
            raise ValueError()
        length = int(length_text)
        if not 0 < length <= MAX_BODY:
            raise RequestError("Request too large", 413)
        if len(body) > length:
            raise ValueError()
    except (ValueError, UnicodeError) as error:
        if isinstance(error, RequestError):
            raise
        raise RequestError() from None
    deadline = time.monotonic() + BODY_TIMEOUT
    while len(body) < length:
        connection.settimeout(_remaining(deadline, BODY_TIMEOUT))
        chunk = connection.recv(length - len(body))
        if not chunk:
            raise RequestError()
        body += chunk
    return body


class _BoundedResponseReader:
    def __init__(self, source):
        self.source = source
        self.headers = True
        self.header_bytes = 0
        self.wire_bytes = 0

    def _count(self, data):
        self.wire_bytes += len(data)
        if self.headers:
            self.header_bytes += len(data)
        if self.header_bytes > MAX_HEADERS or self.wire_bytes > MAX_REPLY + MAX_HEADERS:
            raise UpstreamError()
        return data

    def readline(self, size=-1):
        data = self._count(self.source.readline(min(size, MAX_EVENT + 1) if size >= 0 else MAX_EVENT + 1))
        if len(data) > MAX_EVENT:
            raise UpstreamError()
        return data

    def read(self, size=-1):
        return self._count(self.source.read(min(size, MAX_REPLY + 1) if size >= 0 else MAX_REPLY + 1))

    def read1(self, size=-1):
        return self._count(self.source.read1(min(size, 4096) if size >= 0 else 4096))

    def __getattr__(self, name):
        return getattr(self.source, name)


class _BoundedResponse(http.client.HTTPResponse):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.fp = _BoundedResponseReader(self.fp)

    def begin(self):
        super().begin()
        self.fp.headers = False


class _Events:
    """Bound SSE parsing to one line/event and expose only public text deltas."""
    def __init__(self):
        self.line = bytearray()
        self.data = []
        self.event_bytes = 0
        self.error_event = False
        self.done = False

    def feed(self, chunk):
        for byte in chunk:
            if self.done:
                return
            if byte != 10:
                self.line.append(byte)
                if len(self.line) > MAX_EVENT:
                    raise UpstreamError()
                continue
            line = bytes(self.line).rstrip(b"\r")
            self.line.clear()
            if not line:
                if self.error_event:
                    raise UpstreamError()
                if self.data:
                    data = b"\n".join(self.data)
                    self.data.clear()
                    self.event_bytes = 0
                    if data == b"[DONE]":
                        self.done = True
                        yield b"data: [DONE]\n\n"
                        return
                    try:
                        value = _json(data)
                        if not isinstance(value, dict) or value.get("error") is not None:
                            raise ValueError()
                        choices = value.get("choices", [])
                        if not isinstance(choices, list):
                            raise ValueError()
                        if choices:
                            choice = choices[0]
                            if not isinstance(choice, dict) or not isinstance(choice.get("delta", {}), dict):
                                raise ValueError()
                            content = choice.get("delta", {}).get("content")
                            if content is not None and not isinstance(content, str):
                                raise ValueError()
                            if content:
                                # Splitting by codepoint preserves UTF-8 and keeps
                                # even escaped surrogate pairs under ESP's 8 KiB
                                # event limit. No whole response is accumulated.
                                for start in range(0, len(content), 512):
                                    payload = {"choices": [{"index": 0, "delta": {"content": content[start:start + 512]}}]}
                                    yield b"data: " + json.dumps(payload, ensure_ascii=True, separators=(",", ":")).encode("ascii") + b"\n\n"
                    except (ValueError, TypeError, UnicodeError, RecursionError):
                        raise UpstreamError() from None
                self.error_event = False
            elif line.startswith(b"data:"):
                part = line[5:]
                if part.startswith(b" "):
                    part = part[1:]
                self.event_bytes += len(part) + 1
                if self.event_bytes > MAX_EVENT:
                    raise UpstreamError()
                self.data.append(part)
            elif line.startswith(b"event:") and line[6:].strip() == b"error":
                self.error_event = True


def _send(connection, data, deadline=None):
    connection.settimeout(_remaining(deadline, WRITE_TIMEOUT) if deadline is not None else WRITE_TIMEOUT)
    connection.sendall(data)


def _error_response(connection, status):
    reason = {400: "Bad Request", 401: "Unauthorized", 404: "Not Found", 405: "Method Not Allowed",
              408: "Request Timeout", 413: "Content Too Large", 415: "Unsupported Media Type",
              431: "Request Header Fields Too Large", 502: "Bad Gateway", 503: "Service Unavailable"}.get(status, "Bad Request")
    body = json.dumps({"error": {"message": reason}}, separators=(",", ":")).encode("ascii")
    headers = (f"HTTP/1.1 {status} {reason}\r\nContent-Type: application/json\r\n"
               f"Content-Length: {len(body)}\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n").encode("ascii")
    _send(connection, headers + body)


class _Handler(socketserver.BaseRequestHandler):
    def handle(self):
        connection = None
        streaming = False
        acquired = False
        upstream = None
        response = None
        deadline_timer = None
        try:
            self.request.settimeout(HEADER_TIMEOUT)
            connection = self.server.tls_context.wrap_socket(self.request, server_side=True)
            raw = _receive_request(connection, self.server.device_token)
            body = validate_request(raw, self.server.config.device_identity)
            acquired = self.server.forward_slot.acquire(blocking=False)
            if not acquired:
                raise RequestError("Busy", 503)
            deadline = time.monotonic() + TOTAL_TIMEOUT
            # HTTPConnection uses direct sockets, never environment proxies, and
            # does not replay a failed request or follow a redirect.
            upstream = http.client.HTTPConnection("127.0.0.1", self.server.config.upstream_port,
                                                  timeout=_remaining(deadline, IO_TIMEOUT))
            upstream.response_class = _BoundedResponse
            upstream.connect()
            # A hard deadline also interrupts a slow-drip status/header or
            # chunk-framing read inside http.client, where per-read socket
            # timeouts alone would keep being refreshed by incoming bytes.
            upstream_socket = upstream.sock
            deadline_timer = threading.Timer(_remaining(deadline, TOTAL_TIMEOUT),
                                             self._interrupt, (upstream_socket,))
            deadline_timer.daemon = True
            deadline_timer.start()
            upstream.request("POST", CHAT_PATH, body=body, headers={
                "Authorization": "Bearer " + self.server.upstream_token,
                "Content-Type": "application/json", "Accept": "text/event-stream", "Connection": "close"})
            response = upstream.getresponse()
            if response.status != 200 or response.getheader("Content-Type", "").split(";", 1)[0].strip().lower() != "text/event-stream":
                raise UpstreamError()
            _send(connection, b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", deadline)
            streaming = True
            events = _Events()
            count = 0
            while not events.done:
                # The response keeps ownership of a close-delimited socket even
                # when HTTPConnection.getresponse() clears its sock attribute.
                upstream_socket.settimeout(_remaining(deadline, IO_TIMEOUT))
                chunk = response.read1(4096)
                if not chunk:
                    raise UpstreamError()
                count += len(chunk)
                if count > MAX_REPLY:
                    raise UpstreamError()
                for event in events.feed(chunk):
                    _send(connection, event, deadline)
        except RequestError as error:
            if connection is not None:
                self._safe_error(connection, error.status)
        except (OSError, ValueError, http.client.HTTPException, TimeoutError):
            if connection is not None:
                if streaming:
                    try:
                        _send(connection, b'event: error\ndata: {"error":{"message":"Relay stream ended; agent outcome unknown"}}\n\n')
                    except OSError:
                        pass
                else:
                    self._safe_error(connection, 502 if upstream is not None else 408)
        finally:
            if deadline_timer is not None:
                deadline_timer.cancel()
            if response is not None:
                response.close()
            if upstream is not None:
                upstream.close()
            if acquired:
                self.server.forward_slot.release()
            if connection is not None:
                connection.close()

    @staticmethod
    def _interrupt(connection):
        try:
            connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass

    @staticmethod
    def _safe_error(connection, status):
        try:
            _error_response(connection, status)
        except OSError:
            pass


class RelayServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    allow_reuse_address = True
    daemon_threads = True
    request_queue_size = 4

    def __init__(self, address, config, tls_context):
        self.config = config
        self.tls_context = tls_context
        self.device_token, self.upstream_token = _credentials(config)
        self.client_slots = threading.BoundedSemaphore(MAX_CLIENTS)
        self.forward_slot = threading.BoundedSemaphore(1)
        super().__init__(address, _Handler)

    def process_request(self, request, client_address):
        if not self.client_slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.client_slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            super().process_request_thread(request, client_address)
        finally:
            self.client_slots.release()

    def handle_error(self, request, client_address):
        # Do not print request objects, tracebacks, headers, bodies or credentials.
        print("relay: client processing failed", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", required=True, help="Nonsecret JSON configuration path")
    parser.add_argument("--check-config", action="store_true", help="Validate files and TLS without binding or contacting OpenClaw")
    args = parser.parse_args()
    try:
        config = load_config(args.config)
        if args.check_config:
            print("Relay configuration and TLS files validated; no network connection made.")
            return 0
        context = create_tls_context(config)
        with RelayServer((config.bind_host, config.bind_port), config, context) as server:
            print(f"Relay listening on https://{config.bind_host}:{config.bind_port}{CHAT_PATH}; fixed agent main.", flush=True)
            server.serve_forever()
    except ConfigError as error:
        print(f"relay: {error}", file=sys.stderr)
        return 2
    except OSError:
        print("relay: listener failed; check the interface, port, and permissions", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
