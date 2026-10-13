"""Loopback contract tests; no live Hardware One/OpenClaw service is contacted."""

import base64
from contextlib import contextmanager
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "hardwareone-device" / "scripts" / "hardwareone_client.py"
SPEC = importlib.util.spec_from_file_location("hardwareone_client", SCRIPT)
hw = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = hw
SPEC.loader.exec_module(hw)
TEST_USER = "test-reader"
TEST_PASSWORD = "test-only-password"


@contextmanager
def device_server(responder=None, tls_context=None):
    seen = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            seen.append({"path": self.path, "auth": self.headers.get("Authorization"),
                         "encoding": self.headers.get("Accept-Encoding")})
            try:
                if responder:
                    responder(self)
                else:
                    send_json(self, {"path": self.path, "valid": True})
            except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                pass  # Deliberate timeout/size rejection tests close the socket.

        def log_message(self, *_args):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    if tls_context:
        server.socket = tls_context.wrap_socket(server.socket, server_side=True)
    worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.02}, daemon=True)
    worker.start()
    scheme = "https" if tls_context else "http"
    try:
        yield f"{scheme}://localhost:{server.server_port}", seen
    finally:
        server.shutdown()
        server.server_close()
        worker.join(timeout=1)


def send_json(handler, data, status=200):
    payload = json.dumps(data).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(payload)))
    handler.end_headers()
    handler.wfile.write(payload)


def config(url="https://localhost", **kwargs):
    values = {"device_url": url, "username": TEST_USER, "password": TEST_PASSWORD}
    values.update(kwargs)
    return hw.Config(**values)


class ClientTests(unittest.TestCase):
    def test_device_info_gets_only_the_four_read_routes_with_basic_auth(self):
        with device_server() as (url, seen):
            result = hw.HardwareOneClient(config(url, allow_http=True)).device_info()
        self.assertTrue(result["ok"])
        self.assertEqual([entry["path"] for entry in seen], [path for _, path in hw.DEVICE_INFO_ROUTES])
        expected = "Basic " + base64.b64encode(f"{TEST_USER}:{TEST_PASSWORD}".encode()).decode()
        self.assertTrue(all(entry["auth"] == expected for entry in seen))
        self.assertTrue(all(entry["encoding"] == "identity" for entry in seen))
        self.assertEqual(result["data"]["devices"]["path"], "/api/devices")

    def test_each_allowed_sensor_uses_its_exact_query(self):
        with device_server() as (url, seen):
            client = hw.HardwareOneClient(config(url, allow_http=True))
            for sensor in hw.SENSORS:
                self.assertEqual(client.read_sensor(sensor)["sensor"], sensor)
        self.assertEqual([entry["path"] for entry in seen], ["/api/sensors?sensor=" + s for s in hw.SENSORS])

    def test_invalid_sensor_and_arbitrary_route_do_not_contact_server(self):
        with device_server() as (url, seen):
            client = hw.HardwareOneClient(config(url, allow_http=True))
            for sensor in ("imu&cmd=reboot", "../api/cli", "", "IMU", "apds"):
                with self.assertRaises(hw.ClientError) as caught:
                    client.read_sensor(sensor)
                self.assertEqual(caught.exception.code, "invalid_sensor")
            with self.assertRaises(hw.ClientError):
                client._get_json("/api/cli")
        self.assertEqual(seen, [])

    def test_plaintext_requires_literal_opt_in(self):
        for value in ("", "true", "yes", "0"):
            cfg = hw.Config.from_env({"HW1_DEVICE_URL": "http://localhost", "HW1_USERNAME": TEST_USER,
                                      "HW1_PASSWORD": TEST_PASSWORD, "HW1_ALLOW_HTTP": value})
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(cfg)
            self.assertEqual(caught.exception.code, "plaintext_disabled")

    def test_unsafe_urls_and_bad_credentials_are_rejected_without_echo(self):
        urls = ("", "http://", "file:///private/data", "https://localhost/api",
                "https://localhost?x=1", "https://localhost/#x", "https://localhost:bad",
                "https://name:private@localhost", "https://localhost\n")
        for url in urls:
            with self.subTest(url=url), self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url))
            self.assertNotIn("private@", str(caught.exception))
        for changes in ({"username": ""}, {"password": ""}, {"username": "a:b"}, {"password": "x\ny"}):
            with self.assertRaises(hw.ClientError):
                hw.HardwareOneClient(config(**changes))

    def test_bad_timeouts_fail_before_network(self):
        for timeout in (float("nan"), float("inf"), -1, 0, 31):
            with self.assertRaises(hw.ClientError):
                hw.HardwareOneClient(config(timeout=timeout))
        with self.assertRaises(hw.ClientError):
            hw.Config.from_env({"HW1_TIMEOUT_SECONDS": "not-a-number"})

    def test_redirect_is_not_followed_or_retried(self):
        def redirect(handler):
            handler.send_response(302)
            handler.send_header("Location", "/api/sensors?sensor=gps")
            handler.send_header("Content-Length", "0")
            handler.end_headers()
        with device_server(redirect) as (url, seen):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
        self.assertEqual(caught.exception.code, "redirect_refused")
        self.assertEqual(len(seen), 1)

    def test_http_error_does_not_echo_remote_body_or_retry(self):
        def rejected(handler):
            send_json(handler, {"error": "echo " + TEST_PASSWORD}, status=401)
        with device_server(rejected) as (url, seen):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True)).device_info()
        self.assertEqual(caught.exception.result()["http_status"], 401)
        self.assertNotIn(TEST_PASSWORD, json.dumps(caught.exception.result()))
        self.assertEqual(len(seen), 1)

    def test_unavailable_sensor_is_explicit(self):
        with device_server(lambda h: send_json(h, {"error": "not_compiled"})) as (url, _):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
        self.assertEqual(caught.exception.code, "not_compiled")

    def test_unknown_device_error_text_is_not_echoed(self):
        with device_server(lambda h: send_json(h, {"error": TEST_PASSWORD})) as (url, _):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
        self.assertEqual(caught.exception.code, "device_error")
        self.assertNotIn(TEST_PASSWORD, json.dumps(caught.exception.result()))

    def test_invalid_sample_flags_are_preserved(self):
        sample = {"valid": False, "connected": False, "enabled": False, "ts": 0}
        with device_server(lambda h: send_json(h, sample)) as (url, _):
            result = hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
        self.assertEqual(result["data"], sample)

    def test_declared_oversized_response_is_rejected(self):
        def oversized(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "application/json")
            handler.send_header("Content-Length", str(hw.MAX_RESPONSE_BYTES + 1))
            handler.end_headers()
        with device_server(oversized) as (url, _):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
        self.assertEqual(caught.exception.code, "response_too_large")

    def test_chunked_oversized_response_is_rejected(self):
        def oversized(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "application/json")
            handler.send_header("Transfer-Encoding", "chunked")
            handler.end_headers()
            chunk = b"x" * (hw.MAX_RESPONSE_BYTES + 10)
            handler.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n0\r\n\r\n")
        with device_server(oversized) as (url, _):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
        self.assertEqual(caught.exception.code, "response_too_large")

    def test_bad_json_type_and_encoding_fail(self):
        cases = [
            (b"not-json", "application/json", None, "invalid_response"),
            (b"[]", "application/json", None, "invalid_response"),
            (b'{"n":NaN}', "application/json", None, "invalid_response"),
            (b'{"n":1e999}', "application/json", None, "invalid_response"),
            (b'\xff', "application/json", None, "invalid_response"),
            (b"{}", "text/html", None, "invalid_response"),
            (b"{}", "application/json", "gzip", "invalid_response"),
        ]
        for payload, mime, encoding, code in cases:
            def bad(handler):
                handler.send_response(200)
                handler.send_header("Content-Type", mime)
                handler.send_header("Content-Length", str(len(payload)))
                if encoding:
                    handler.send_header("Content-Encoding", encoding)
                handler.end_headers()
                handler.wfile.write(payload)
            with self.subTest(payload=payload, mime=mime, encoding=encoding):
                with device_server(bad) as (url, _):
                    with self.assertRaises(hw.ClientError) as caught:
                        hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
                self.assertEqual(caught.exception.code, code)

    def test_truncated_response_cannot_be_reported_as_success(self):
        def truncated(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "application/json")
            handler.send_header("Content-Length", "20")
            handler.send_header("Connection", "close")
            handler.end_headers()
            handler.wfile.write(b"{}")
        with device_server(truncated) as (url, _):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True)).read_sensor("imu")
        self.assertEqual(caught.exception.code, "invalid_response")

    def test_delayed_headers_timeout_without_retry(self):
        def slow(handler):
            time.sleep(0.35)
            send_json(handler, {})
        with device_server(slow) as (url, seen):
            start = time.monotonic()
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True, timeout=0.1)).read_sensor("imu")
            self.assertLess(time.monotonic() - start, 0.8)
        self.assertEqual(caught.exception.code, "timeout")
        self.assertEqual(len(seen), 1)

    def test_trickling_body_has_a_total_connected_deadline(self):
        def trickle(handler):
            handler.send_response(200)
            handler.send_header("Content-Type", "application/json")
            handler.send_header("Content-Length", "101")
            handler.end_headers()
            for _ in range(101):
                handler.wfile.write(b" ")
                handler.wfile.flush()
                time.sleep(0.02)
        with device_server(trickle) as (url, _):
            start = time.monotonic()
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, allow_http=True, timeout=0.15)).read_sensor("imu")
            self.assertLess(time.monotonic() - start, 0.8)
        self.assertEqual(caught.exception.code, "timeout")

    def test_cli_emits_one_json_result_and_reads_credentials_from_environment(self):
        with device_server() as (url, seen):
            env = {"HW1_DEVICE_URL": url, "HW1_USERNAME": TEST_USER, "HW1_PASSWORD": TEST_PASSWORD,
                   "HW1_ALLOW_HTTP": "1"}
            proc = subprocess.run([sys.executable, str(SCRIPT), "read_sensor", "imu"],
                                  env=env, text=True, capture_output=True, timeout=5)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(json.loads(proc.stdout)["sensor"], "imu")
        self.assertEqual(proc.stderr, "")
        self.assertNotIn(TEST_PASSWORD, proc.stdout)
        self.assertEqual(len(seen), 1)

    def test_cli_configuration_error_has_no_traceback_or_secret(self):
        proc = subprocess.run([sys.executable, str(SCRIPT), "read_sensor", "imu"],
                              env={"HW1_PASSWORD": TEST_PASSWORD}, text=True, capture_output=True, timeout=5)
        self.assertEqual(proc.returncode, 1)
        self.assertEqual(json.loads(proc.stdout)["error"], "configuration")
        self.assertEqual(proc.stderr, "")
        self.assertNotIn(TEST_PASSWORD, proc.stdout)

    def test_output_limit_fails_with_json_instead_of_partial_output(self):
        oversized = {"ok": True, "data": {"large": "x" * hw.MAX_OUTPUT_BYTES}}
        with mock.patch.object(hw.Config, "from_env", return_value=config()), \
                mock.patch.object(hw.HardwareOneClient, "device_info", return_value=oversized), \
                mock.patch("sys.stdout", new_callable=io.StringIO) as output:
            code = hw.main(["device_info"])
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(output.getvalue())["error"], "output_too_large")


@unittest.skipUnless(shutil.which("openssl"), "openssl is needed only to generate the temporary test certificate")
class TLSTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="hw1-client-tls-")
        root = Path(cls.temp.name)
        cls.cert, cls.key = root / "cert.pem", root / "key.pem"
        configuration = root / "openssl.cnf"
        configuration.write_text(
            "[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=ext\n"
            "[dn]\nCN=localhost\n[ext]\nsubjectAltName=DNS:localhost\n"
            "basicConstraints=critical,CA:TRUE\nkeyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\n"
        )
        subprocess.run([shutil.which("openssl"), "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-days", "1", "-config", str(configuration), "-keyout", str(cls.key),
                        "-out", str(cls.cert)], capture_output=True, check=True, timeout=15)
        cls.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        cls.context.load_cert_chain(str(cls.cert), str(cls.key))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_trusted_certificate_succeeds(self):
        with device_server(tls_context=self.context) as (url, seen):
            result = hw.HardwareOneClient(config(url, ca_file=str(self.cert))).read_sensor("imu")
        self.assertTrue(result["ok"])
        self.assertEqual(len(seen), 1)

    def test_untrusted_certificate_fails_before_credentials_are_sent(self):
        with device_server(tls_context=self.context) as (url, seen):
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url)).read_sensor("imu")
        self.assertEqual(caught.exception.code, "tls_verification_failed")
        self.assertEqual(seen, [])

    def test_wrong_hostname_fails_even_with_trusted_certificate(self):
        with device_server(tls_context=self.context) as (url, seen):
            url = url.replace("localhost", "127.0.0.1")
            with self.assertRaises(hw.ClientError) as caught:
                hw.HardwareOneClient(config(url, ca_file=str(self.cert))).read_sensor("imu")
        self.assertEqual(caught.exception.code, "tls_verification_failed")
        self.assertEqual(seen, [])


if __name__ == "__main__":
    unittest.main()
