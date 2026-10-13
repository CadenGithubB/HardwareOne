"""Restricted relay boundary tests; all network traffic stays on loopback."""

from contextlib import contextmanager
from dataclasses import asdict
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "relay" / "relay.py"
SPEC = importlib.util.spec_from_file_location("hardwareone_relay", SCRIPT)
relay = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = relay
SPEC.loader.exec_module(relay)

# Deliberately fake credentials used only by these local fixtures.
DEVICE_TOKEN = "test-device-token-" + "d" * 40
UPSTREAM_TOKEN = "test-upstream-token-" + "u" * 40
CONVERSATION = "hw1:" + "0123456789abcdef" * 2
DEVICE_IDENTITY = "test-xiao"


def request_body(**changes):
    value = {"model": "openclaw/main", "user": CONVERSATION, "stream": True,
             "messages": [{"role": "user", "content": "Hello, Mingus."}]}
    value.update(changes)
    return json.dumps(value, ensure_ascii=False).encode("utf-8")


def sse_content(text):
    return ("data: " + json.dumps({"choices": [{"delta": {"content": text}}]},
                                 ensure_ascii=False) + "\n\n").encode("utf-8")


SSE_DONE = b"data: [DONE]\n\n"


def upstream_response(handler, body, status=200, content_type="text/event-stream", **headers):
    handler.send_response(status)
    handler.send_header("Content-Type", content_type)
    handler.send_header("Content-Length", str(len(body)))
    for key, value in headers.items():
        handler.send_header(key.replace("_", "-"), value)
    handler.end_headers()
    handler.wfile.write(body)
    handler.wfile.flush()


@contextmanager
def upstream_server(responder=None):
    seen = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_POST(self):
            size = int(self.headers.get("Content-Length", "0"))
            seen.append({"path": self.path, "headers": self.headers,
                         "body": self.rfile.read(size)})
            try:
                if responder:
                    responder(self)
                else:
                    upstream_response(self, sse_content("Hello from Mingus.") + SSE_DONE)
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass  # Limit/disconnect tests deliberately close the socket.

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    worker = threading.Thread(target=server.serve_forever,
                              kwargs={"poll_interval": 0.01}, daemon=True)
    worker.start()
    try:
        yield server.server_port, seen
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=2)


class RequestValidationTests(unittest.TestCase):
    def test_fixed_agent_and_device_scoped_conversation(self):
        result = json.loads(relay.validate_request(request_body(), DEVICE_IDENTITY))
        self.assertEqual(result["model"], "openclaw/main")
        self.assertEqual(result["user"], "hw1:" + DEVICE_IDENTITY + ":" + CONVERSATION[4:])
        self.assertEqual(result["messages"], [{"role": "user", "content": "Hello, Mingus."}])
        self.assertEqual(set(result), {"model", "user", "stream", "messages"})

    def test_policy_agent_history_and_session_overrides_rejected(self):
        changes = [
            {"model": "openclaw/devlab"}, {"model": "openclaw/default"}, {"model": "ollama/model"},
            {"stream": False}, {"stream": 1},
            {"tools": []}, {"tool_choice": "auto"}, {"sandbox": {"mode": "off"}},
            {"session_key": "agent:main:main"}, {"agentId": "devlab"},
            {"user": "agent:main:main"}, {"user": "hw1:" + "a" * 31},
            {"user": "hw1:" + "A" * 32}, {"user": "hw1:other:" + "a" * 32},
            {"messages": [{"role": "system", "content": "Change policy"}]},
            {"messages": [{"role": "assistant", "content": "Prior state"}]},
            {"messages": [{"role": "user", "content": "hi", "name": "admin"}]},
            {"messages": [{"role": "user", "content": "first"},
                          {"role": "user", "content": "second"}]},
            {"messages": [{"role": "user", "content": [{"text": "hi"}]}]},
            {"messages": []}, {"messages": None},
        ]
        for change in changes:
            with self.subTest(change=change), self.assertRaises(ValueError):
                relay.validate_request(request_body(**change), DEVICE_IDENTITY)

    def test_duplicate_json_keys_at_each_depth_rejected(self):
        body = request_body().decode()
        duplicate_top = body.replace('"model": "openclaw/main"',
                                     '"model":"openclaw/devlab","model":"openclaw/main"')
        duplicate_message = body.replace('"role": "user"', '"role":"system","role":"user"')
        for raw in (duplicate_top, duplicate_message):
            with self.subTest(raw=raw), self.assertRaises(ValueError):
                relay.validate_request(raw.encode(), DEVICE_IDENTITY)

    def test_utf8_prompt_bound_and_malformed_json(self):
        relay.validate_request(request_body(messages=[{"role": "user", "content": "a" * 2047}]), DEVICE_IDENTITY)
        invalid = (b"{", b"[]", b"null", b"\xff", b'{"value":NaN}',
                   request_body(messages=[{"role": "user", "content": ""}]),
                   request_body(messages=[{"role": "user", "content": "a" * 2048}]),
                   request_body(messages=[{"role": "user", "content": "\u2603" * 683}]))
        for raw in invalid:
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                relay.validate_request(raw, DEVICE_IDENTITY)


@unittest.skipUnless(shutil.which("openssl"), "openssl is needed for disposable loopback TLS fixtures")
class RelayBoundaryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="hw1-relay-test-")
        cls.root = Path(cls.tmp.name)
        cls.cert = cls.root / "cert.pem"
        cls.key = cls.root / "key.pem"
        config = cls.root / "openssl.cnf"
        config.write_text("[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n"
                          "[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1\n")
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                        "-config", str(config), "-keyout", str(cls.key), "-out", str(cls.cert)],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.device_token = cls.root / "device-token"
        cls.upstream_token = cls.root / "upstream-token"
        cls.device_token.write_text(DEVICE_TOKEN + "\n")
        cls.upstream_token.write_text(UPSTREAM_TOKEN + "\n")
        for path in (cls.key, cls.device_token, cls.upstream_token):
            path.chmod(0o600)
        cls.client_tls = ssl.create_default_context(cafile=str(cls.cert))
        cls.server_tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.server_tls.load_cert_chain(str(cls.cert), str(cls.key))

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def relay_config(self, upstream_port, **changes):
        values = dict(bind_host="127.0.0.1", bind_port=18443, device_identity=DEVICE_IDENTITY,
                      tls_cert_file=str(self.cert), tls_key_file=str(self.key),
                      device_token_file=str(self.device_token),
                      upstream_token_file=str(self.upstream_token), upstream_port=upstream_port)
        values.update(changes)
        return relay.Config(**values)

    @contextmanager
    def running_relay(self, upstream_port):
        config = self.relay_config(upstream_port)
        server = relay.RelayServer(("127.0.0.1", 0), config, self.server_tls)
        worker = threading.Thread(target=server.serve_forever,
                                  kwargs={"poll_interval": 0.01}, daemon=True)
        worker.start()
        try:
            yield server.server_address[1]
        finally:
            server.shutdown()
            server.server_close()
            worker.join(timeout=2)

    def call(self, port, body=None, path="/v1/chat/completions", method="POST", token=DEVICE_TOKEN, headers=None):
        body = request_body() if body is None else body
        values = {"Content-Type": "application/json", "Accept": "text/event-stream"}
        if token is not None:
            values["Authorization"] = "Bearer " + token
        values.update(headers or {})
        connection = http.client.HTTPSConnection("localhost", port, timeout=3, context=self.client_tls)
        try:
            connection.putrequest(method, path, skip_accept_encoding=True)
            for name, value in values.items():
                connection.putheader(name, value)
            connection.putheader("Content-Length", str(len(body)))
            connection.endheaders(body)
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def raw_call(self, port, request):
        chunks = []
        with socket.create_connection(("127.0.0.1", port), timeout=3) as raw:
            with self.client_tls.wrap_socket(raw, server_hostname="localhost") as secure:
                secure.sendall(request)
                while True:
                    try:
                        data = secure.recv(65536)
                    except (ConnectionResetError, ssl.SSLError):
                        break
                    if not data:
                        break
                    chunks.append(data)
        return b"".join(chunks)

    def test_tls_auth_and_stream_use_distinct_upstream_credential(self):
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            status, headers, body = self.call(port)
        self.assertEqual(status, 200)
        self.assertIn("text/event-stream", headers.get("Content-Type", ""))
        self.assertIn(b"Hello from Mingus.", body)
        self.assertIn(SSE_DONE, body)
        self.assertEqual(len(seen), 1)
        self.assertEqual(seen[0]["path"], "/v1/chat/completions")
        self.assertEqual(seen[0]["headers"]["Authorization"], "Bearer " + UPSTREAM_TOKEN)
        forwarded = json.loads(seen[0]["body"])
        self.assertEqual(forwarded["model"], "openclaw/main")
        self.assertEqual(forwarded["user"], "hw1:" + DEVICE_IDENTITY + ":" + CONVERSATION[4:])
        for secret in (DEVICE_TOKEN, UPSTREAM_TOKEN):
            self.assertNotIn(secret.encode(), body)

    def test_missing_wrong_or_upstream_token_cannot_reach_gateway(self):
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            for token in (None, "incorrect", UPSTREAM_TOKEN):
                with self.subTest(token=token is None):
                    status, _, _ = self.call(port, token=token)
                    self.assertEqual(status, 401)
        self.assertEqual(seen, [])

    def test_other_paths_methods_content_types_and_policy_fields_never_forward(self):
        bad_calls = [dict(path="/"), dict(path="/v1/chat/completions?agent=devlab"),
                     dict(path="/v1/chat/completions/"), dict(path="/tools/invoke"),
                     dict(method="GET"), dict(method="OPTIONS"),
                     dict(headers={"Content-Type": "text/plain"}),
                     dict(body=request_body(tools=[])),
                     dict(body=request_body(model="openclaw/devlab"))]
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            for kwargs in bad_calls:
                with self.subTest(kwargs=kwargs):
                    status, _, body = self.call(port, **kwargs)
                    self.assertGreaterEqual(status, 400)
                    self.assertNotIn(UPSTREAM_TOKEN.encode(), body)
        self.assertEqual(seen, [])

    def test_injection_headers_never_reach_gateway(self):
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            status, _, _ = self.call(port, headers={"x-openclaw-agent-id": "devlab",
                                                  "x-openclaw-session-key": "agent:main:main",
                                                  "Cookie": "session=pretend-admin"})
        if status == 200:
            self.assertEqual(len(seen), 1)
            for name in ("x-openclaw-agent-id", "x-openclaw-session-key", "Cookie"):
                self.assertIsNone(seen[0]["headers"].get(name))
        else:
            self.assertGreaterEqual(status, 400)
            self.assertEqual(seen, [])

    def test_ambiguous_http_framing_and_duplicate_auth_rejected(self):
        body = request_body()
        prefix = ("POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\n"
                  "Content-Type: application/json\r\nAuthorization: Bearer " + DEVICE_TOKEN + "\r\n")
        cases = ["Content-Length: " + str(len(body)) + "\r\nContent-Length: " + str(len(body)),
                 "Transfer-Encoding: chunked\r\nContent-Length: " + str(len(body)),
                 "Content-Length: " + str(len(body)) + "\r\nAuthorization: Bearer " + DEVICE_TOKEN,
                 "Content-Length: -1", "Content-Length: 999999999", "Transfer-Encoding: chunked"]
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            for framing in cases:
                with self.subTest(framing=framing):
                    response = self.raw_call(port, (prefix + framing + "\r\n\r\n").encode() + body)
                    self.assertTrue(response.startswith(b"HTTP/1.1 4"), response[:80])
        self.assertEqual(seen, [])

    def test_redirects_and_upstream_errors_are_fixed_not_retried(self):
        for status in (301, 307, 401, 500):
            def rejected(handler):
                upstream_response(handler, ("private diagnostic " + UPSTREAM_TOKEN).encode(),
                                  status=status, content_type="text/plain", Location="/tools/invoke")
            with self.subTest(status=status), upstream_server(rejected) as (upstream, seen), self.running_relay(upstream) as port:
                result, headers, body = self.call(port)
                self.assertGreaterEqual(result, 400)
                self.assertNotIn("Location", headers)
                self.assertNotIn(b"private diagnostic", body)
                self.assertNotIn(UPSTREAM_TOKEN.encode(), body)
            self.assertEqual(len(seen), 1)

    def test_proxy_environment_is_not_used(self):
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            with mock.patch.dict(os.environ, {"HTTP_PROXY": "http://127.0.0.1:1", "HTTPS_PROXY": "http://127.0.0.1:1",
                                              "ALL_PROXY": "http://127.0.0.1:1", "NO_PROXY": ""}):
                status, _, _ = self.call(port)
        self.assertEqual(status, 200)
        self.assertEqual(len(seen), 1)

    def test_tools_thinking_metadata_and_raw_error_are_not_returned(self):
        events = [{"choices": [{"delta": {"role": "assistant", "reasoning_content": "private thought",
                                            "tool_calls": [{"function": {"name": "dangerous", "arguments": UPSTREAM_TOKEN}}]}}]},
                  {"choices": [{"delta": {"content": "Visible \u2603"}}], "internal": UPSTREAM_TOKEN}]
        raw = b"".join(("data: " + json.dumps(event) + "\n\n").encode() for event in events) + SSE_DONE
        with upstream_server(lambda h: upstream_response(h, raw)) as (upstream, _), self.running_relay(upstream) as port:
            status, _, body = self.call(port)
        self.assertEqual(status, 200)
        public_chunks = [json.loads(line[6:]) for line in body.decode().splitlines()
                         if line.startswith("data: ") and line != "data: [DONE]"]
        self.assertIn("Visible \u2603", "".join(chunk["choices"][0]["delta"].get("content", "")
                                             for chunk in public_chunks))
        for private in ("private thought", "dangerous", "tool_calls", "internal", UPSTREAM_TOKEN):
            self.assertNotIn(private.encode(), body)

    def test_broken_sse_does_not_fabricate_success_or_expose_error(self):
        cases = [sse_content("partial"), b"data: not-json\n\n", b"data: {\"error\":\"" + UPSTREAM_TOKEN.encode() + b"\"}\n\n",
                 b"event: error\ndata: " + UPSTREAM_TOKEN.encode() + b"\n\n", b"data: " + b"x" * 9000 + b"\n\n"]
        for payload in cases:
            with self.subTest(size=len(payload)), upstream_server(lambda h: upstream_response(h, payload)) as (upstream, seen), self.running_relay(upstream) as port:
                status, _, body = self.call(port)
                self.assertEqual(status, 200)
                self.assertIn(b"event: error", body)
                self.assertNotIn(SSE_DONE, body)
                self.assertNotIn(UPSTREAM_TOKEN.encode(), body)
            self.assertEqual(len(seen), 1)

    def test_concurrent_generation_is_refused_without_a_second_gateway_call(self):
        started = threading.Event()
        finish = threading.Event()
        first = []

        def delayed(handler):
            started.set()
            finish.wait(3)
            upstream_response(handler, sse_content("complete") + SSE_DONE)

        with upstream_server(delayed) as (upstream, seen), self.running_relay(upstream) as port:
            worker = threading.Thread(target=lambda: first.append(self.call(port)), daemon=True)
            worker.start()
            try:
                self.assertTrue(started.wait(2))
                status, _, _ = self.call(port)
                self.assertIn(status, (429, 503))
                self.assertEqual(len(seen), 1)
            finally:
                finish.set()
                worker.join(timeout=3)
            self.assertEqual(first[0][0], 200)

    def test_stalled_upstream_releases_slot_without_retry(self):
        finish = threading.Event()

        def stalled(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.flush()
            finish.wait(2)

        try:
            with mock.patch.object(relay, "IO_TIMEOUT", 0.15), upstream_server(stalled) as (upstream, seen), self.running_relay(upstream) as port:
                start = time.monotonic()
                status, _, body = self.call(port)
                self.assertEqual(status, 200)
                self.assertIn(b"event: error", body)
                self.assertNotIn(SSE_DONE, body)
                self.assertLess(time.monotonic() - start, 1.5)
                self.assertEqual(len(seen), 1)
        finally:
            finish.set()

    def test_reply_size_bound_reports_failure(self):
        data = sse_content("a" * 256) * 8 + SSE_DONE
        with mock.patch.object(relay, "MAX_REPLY", 512), upstream_server(lambda h: upstream_response(h, data)) as (upstream, seen), self.running_relay(upstream) as port:
            status, _, body = self.call(port)
        self.assertEqual(status, 200)
        self.assertIn(b"event: error", body)
        self.assertNotIn(SSE_DONE, body)
        self.assertLess(len(body), len(data))
        self.assertEqual(len(seen), 1)

    def test_total_deadline_bounds_a_continuously_active_stream(self):
        disconnected = threading.Event()

        def trickling(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Connection", "close")
            handler.end_headers()
            try:
                for _ in range(100):
                    handler.wfile.write(sse_content("x"))
                    handler.wfile.flush()
                    time.sleep(0.02)
            except (BrokenPipeError, ConnectionResetError):
                disconnected.set()

        with mock.patch.object(relay, "TOTAL_TIMEOUT", 0.2), upstream_server(trickling) as (upstream, seen), self.running_relay(upstream) as port:
            start = time.monotonic()
            status, _, body = self.call(port)
            self.assertLess(time.monotonic() - start, 1.5)
            self.assertEqual(status, 200)
            self.assertIn(b"event: error", body)
            self.assertNotIn(SSE_DONE, body)
            self.assertTrue(disconnected.wait(1))
        self.assertEqual(len(seen), 1)

    def test_device_disconnect_closes_gateway_connection(self):
        started = threading.Event()
        disconnected = threading.Event()

        def streaming(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Connection", "close")
            handler.end_headers()
            try:
                for _ in range(150):
                    handler.wfile.write(sse_content("still streaming"))
                    handler.wfile.flush()
                    started.set()
                    time.sleep(0.01)
            except (BrokenPipeError, ConnectionResetError):
                disconnected.set()

        with upstream_server(streaming) as (upstream, seen), self.running_relay(upstream) as port:
            connection = http.client.HTTPSConnection("localhost", port, timeout=3, context=self.client_tls)
            body = request_body()
            connection.putrequest("POST", "/v1/chat/completions", skip_accept_encoding=True)
            connection.putheader("Content-Type", "application/json")
            connection.putheader("Authorization", "Bearer " + DEVICE_TOKEN)
            connection.putheader("Content-Length", str(len(body)))
            connection.endheaders(body)
            response = connection.getresponse()
            self.assertEqual(response.status, 200)
            self.assertTrue(started.wait(1))
            response.close()
            connection.close()
            self.assertTrue(disconnected.wait(2))
        self.assertEqual(len(seen), 1)

    def test_slow_device_backpressure_is_bounded_and_forwarding_slot_recovers(self):
        disconnected = threading.Event()
        streaming = threading.Event()

        def flooding(handler):
            if streaming.is_set():
                upstream_response(handler, sse_content("next request works") + SSE_DONE)
                return
            streaming.set()
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Connection", "close")
            handler.end_headers()
            try:
                for _ in range(16384):
                    handler.wfile.write(sse_content("x" * 4096))
                    handler.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                disconnected.set()

        with mock.patch.object(relay, "WRITE_TIMEOUT", 0.1), mock.patch.object(relay, "MAX_REPLY", 80 * 1024 * 1024):
            with upstream_server(flooding) as (upstream, seen), self.running_relay(upstream) as port:
                raw = socket.create_connection(("127.0.0.1", port), timeout=3)
                raw.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                secure = self.client_tls.wrap_socket(raw, server_hostname="localhost")
                try:
                    body = request_body()
                    header = ("POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\n"
                              "Content-Type: application/json\r\nAuthorization: Bearer " + DEVICE_TOKEN +
                              "\r\nContent-Length: " + str(len(body)) + "\r\n\r\n")
                    secure.sendall(header.encode() + body)
                    # Intentionally do not consume the HTTPS response.
                    self.assertTrue(streaming.wait(1))
                    self.assertTrue(disconnected.wait(2), "blocked device did not close upstream")
                    status, _, response = self.call(port)
                    self.assertEqual(status, 200)
                    self.assertIn(b"next request works", response)
                finally:
                    secure.close()
        self.assertEqual(len(seen), 2)

    def test_untrusted_certificate_is_rejected_and_listener_recovers(self):
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            connection = http.client.HTTPSConnection("localhost", port, timeout=3,
                                                   context=ssl.create_default_context())
            try:
                with self.assertRaises(ssl.SSLCertVerificationError):
                    connection.request("POST", "/v1/chat/completions", body=request_body())
            finally:
                connection.close()
            self.assertEqual(self.call(port)[0], 200)
        self.assertEqual(len(seen), 1)

    def test_config_rejects_remote_targets_wildcard_bind_and_unknown_fields(self):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            path = Path(folder) / "config.json"
            value = asdict(self.relay_config(18789))
            path.write_text(json.dumps(value))
            with mock.patch.object(socket, "socket", side_effect=AssertionError("config must not open network")):
                self.assertEqual(relay.load_config(str(path)).device_identity, DEVICE_IDENTITY)
            for change in ({"upstream_url": "http://other-host:18789"}, {"agent": "devlab"},
                           {"bind_host": "0.0.0.0"}, {"bind_host": "localhost"},
                           {"bind_host": "::"}, {"bind_host": True}, {"bind_host": 123},
                           {"bind_port": True}, {"bind_port": 0},
                           {"upstream_port": 70000}, {"tls_key_file": "relative.pem"},
                           {"device_identity": "../../other"}):
                with self.subTest(change=change):
                    path.write_text(json.dumps(dict(value, **change)))
                    with self.assertRaises(relay.ConfigError):
                        relay.load_config(str(path), check_files=False)
            duplicate = json.dumps(value).replace('"bind_port": 18443', '"bind_port":18443,"bind_port":18444')
            path.write_text(duplicate)
            with self.assertRaises(relay.ConfigError):
                relay.load_config(str(path), check_files=False)

    def test_private_credentials_refuse_loose_modes_symlinks_and_reused_gateway_token(self):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            root = Path(folder)
            token = root / "private-token"
            link = root / "token-link"
            config_path = root / "config.json"
            value = asdict(self.relay_config(18789, device_token_file=str(token)))
            config_path.write_text(json.dumps(value))
            token.write_text(DEVICE_TOKEN)
            token.chmod(0o644)
            with self.assertRaises(relay.ConfigError):
                relay.load_config(str(config_path))
            token.chmod(0o600)
            link.symlink_to(token)
            config_path.write_text(json.dumps(dict(value, device_token_file=str(link))))
            with self.assertRaises(relay.ConfigError):
                relay.load_config(str(config_path))
            config_path.write_text(json.dumps(value))
            for content in (UPSTREAM_TOKEN, "short", "bad token with spaces", "x" * 1025,
                            DEVICE_TOKEN + "\nextra-line", "\u2603" * 20):
                token.write_text(content)
                with self.subTest(length=len(content)), self.assertRaises(relay.ConfigError) as caught:
                    relay.load_config(str(config_path))
                self.assertNotIn(content, str(caught.exception))

    @unittest.skipUnless(hasattr(os, "mkfifo"), "FIFO rejection applies to POSIX hosts")
    def test_credential_fifo_is_rejected_without_waiting_for_a_writer(self):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            fifo = Path(folder) / "not-a-token-file"
            os.mkfifo(str(fifo), 0o600)
            config = Path(folder) / "config.json"
            config.write_text(json.dumps(asdict(self.relay_config(18789, device_token_file=str(fifo)))))
            process = subprocess.run([sys.executable, str(SCRIPT), "--config", str(config), "--check-config"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=1)
            self.assertEqual(process.returncode, 2)
            self.assertIn(b"Credential files", process.stderr)

    def test_existing_gateway_credential_does_not_inherit_new_device_token_minimum(self):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            upstream = Path(folder) / "existing-gateway-token"
            device = Path(folder) / "new-device-token"
            config = Path(folder) / "config.json"
            upstream.write_text("x")
            device.write_text(DEVICE_TOKEN)
            upstream.chmod(0o600)
            device.chmod(0o600)
            config.write_text(json.dumps(asdict(self.relay_config(18789, upstream_token_file=str(upstream),
                                                                device_token_file=str(device)))))
            self.assertEqual(relay.load_config(str(config)).upstream_token_file, str(upstream))
            device.write_text("d" * 31)
            with self.assertRaises(relay.ConfigError):
                relay.load_config(str(config))

    def test_check_config_validates_without_binding_and_without_secret_output(self):
        with tempfile.TemporaryDirectory(dir=self.root) as folder:
            config = Path(folder) / "config.json"
            config.write_text(json.dumps(asdict(self.relay_config(18789))))
            process = subprocess.run([sys.executable, str(SCRIPT), "--config", str(config), "--check-config"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=5)
            self.assertEqual(process.returncode, 0, process.stderr)
            self.assertIn(b"no network connection made", process.stdout)
            for secret in (DEVICE_TOKEN, UPSTREAM_TOKEN):
                self.assertNotIn(secret.encode(), process.stdout + process.stderr)

    def test_incomplete_header_and_body_have_fixed_deadlines(self):
        body = request_body()
        complete_header = ("POST /v1/chat/completions HTTP/1.1\r\nHost: localhost\r\n"
                           "Content-Type: application/json\r\nAuthorization: Bearer " + DEVICE_TOKEN +
                           "\r\nContent-Length: " + str(len(body)) + "\r\n\r\n").encode()
        with mock.patch.object(relay, "HEADER_TIMEOUT", 0.1), mock.patch.object(relay, "BODY_TIMEOUT", 0.1):
            with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
                for payload in (b"POST /v1/chat/completions HTTP/1.1\r\nHost:", complete_header + body[:8]):
                    start = time.monotonic()
                    response = self.raw_call(port, payload)
                    self.assertTrue(response.startswith(b"HTTP/1.1 408"), response[:80])
                    self.assertLess(time.monotonic() - start, 1)
        self.assertEqual(seen, [])

    def test_header_size_limit_rejects_before_contacting_gateway(self):
        payload = b"POST /v1/chat/completions HTTP/1.1\r\nHost: " + b"x" * 9000 + b"\r\n\r\n"
        with upstream_server() as (upstream, seen), self.running_relay(upstream) as port:
            response = self.raw_call(port, payload)
        self.assertTrue(response.startswith(b"HTTP/1.1 431"), response[:80])
        self.assertEqual(seen, [])

    def test_chunked_upstream_preserves_utf8_across_chunk_boundaries(self):
        payload = sse_content("Snow \u2603 and \U0001f99e") + SSE_DONE

        def chunked(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "text/event-stream")
            handler.send_header("Transfer-Encoding", "chunked")
            handler.end_headers()
            for byte in payload:
                handler.wfile.write(b"1\r\n" + bytes([byte]) + b"\r\n")
            handler.wfile.write(b"0\r\n\r\n")
            handler.wfile.flush()

        with upstream_server(chunked) as (upstream, seen), self.running_relay(upstream) as port:
            status, _, body = self.call(port)
        self.assertEqual(status, 200)
        public_chunks = [json.loads(line[6:]) for line in body.decode().splitlines()
                         if line.startswith("data: ") and line != "data: [DONE]"]
        self.assertEqual("".join(chunk["choices"][0]["delta"].get("content", "")
                                 for chunk in public_chunks), "Snow \u2603 and \U0001f99e")
        self.assertIn(SSE_DONE, body)
        self.assertEqual(len(seen), 1)

    def test_slow_drip_upstream_headers_and_chunk_framing_hit_absolute_deadline(self):
        for during_headers in (True, False):
            disconnected = threading.Event()

            def drip(handler):
                if during_headers:
                    handler.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nX-Slow: ")
                else:
                    handler.send_response(200)
                    handler.send_header("Content-Type", "text/event-stream")
                    handler.send_header("Transfer-Encoding", "chunked")
                    handler.end_headers()
                handler.wfile.flush()
                try:
                    for _ in range(200):
                        handler.wfile.write(b"a")  # No line ending; each byte resets socket idle time.
                        handler.wfile.flush()
                        time.sleep(0.01)
                except (BrokenPipeError, ConnectionResetError):
                    disconnected.set()

            with self.subTest(during_headers=during_headers), mock.patch.object(relay, "TOTAL_TIMEOUT", 0.15):
                with upstream_server(drip) as (upstream, seen), self.running_relay(upstream) as port:
                    start = time.monotonic()
                    status, _, body = self.call(port)
                    self.assertLess(time.monotonic() - start, 1)
                    if during_headers:
                        self.assertEqual(status, 502)
                    else:
                        self.assertEqual(status, 200)
                        self.assertIn(b"event: error", body)
                        self.assertNotIn(SSE_DONE, body)
                    self.assertTrue(disconnected.wait(1))
                self.assertEqual(len(seen), 1)


if __name__ == "__main__":
    unittest.main()
