"""HTTP bridge validation tests with a mocked serial endpoint; no hardware."""
import base64
import hashlib
import json
from pathlib import Path
import stat
import tempfile
import unittest
import urllib.parse

from test_http_s3 import HttpBridgeError, parse_bridge, private_encodings, run_probe_http, validate_reply


def response(body=b"", status=200, location="", cookie=False):
    low = body.lower()
    value = {"schema": 1, "bridge": "s3-http-v1", "ok": True, "connected": True,
             "cookiePresent": cookie, "complete": True, "status": status, "location": location,
             "bytes": len(body), "sha256": hashlib.sha256(body).hexdigest(), "durationMs": 12,
             "htmlOpen": b"<html" in low, "htmlClose": b"</html>" in low,
             "hasUsername": b"username" in low, "hasPassword": b"password" in low,
             "bodyIncluded": len(body) <= 8192}
    if value["bodyIncluded"]:
        value["body"] = base64.b64encode(body).decode()
    return value


class FakeConsole:
    def __init__(self, credentials, corrupt=None):
        self.credentials = credentials
        self.commands = []
        self.cookie = False
        self.corrupt = corrupt
    def command(self, command, timeout):
        self.commands.append(command)
        if command == "whoami":
            return "You are " + self.credentials["username"] + " (admin)\n"
        words = command.split()
        op = words[1]
        value = {"schema": 1, "bridge": "s3-http-v1", "ok": True, "connected": True,
                 "cookiePresent": self.cookie, "ip": "192.168.4.2", "apIp": "192.168.5.1", "channel": 6}
        if op == "clear":
            self.cookie = False
            value["cookiePresent"] = False
        elif op == "join":
            assert base64.b64decode(words[2]).decode() == "HW1_P4_TEST"
            assert base64.b64decode(words[3]).decode() == self.credentials["ap_password"]
        elif op == "request":
            method, path = words[2:4]
            if path == "/login" and method == "GET":
                value = response(b'<html><input name="username"><input name="password"></html>')
            elif path == "/login":
                fields = urllib.parse.parse_qs(base64.b64decode(words[4]).decode())
                assert fields["username"] == [self.credentials["username"]]
                assert fields["password"] == [self.credentials["password"]]
                self.cookie = True
                value = response(status=303, location="/dashboard", cookie=True)
            elif path == "/logout":
                self.cookie = False
                value = response(status=302, location="/login")
            elif not self.cookie:
                value = response(b'{"error":"unauthorized"}', status=401)
            elif path == "/api/cli":
                fields = urllib.parse.parse_qs(base64.b64decode(words[4]).decode())
                body = ("You are " + self.credentials["username"] + " (admin)").encode() if fields["cmd"] == ["whoami"] else b'prefix {"schema":1,"initialized":true}'
                value = response(body, cookie=True)
            elif path.startswith("/api/"):
                value = response(b'{"schema":1,"fw":"7"}', cookie=True)
            else:
                # Exercise full streamed-page summary with no USB payload.
                value = response(b"<html>" + b"x" * 9000 + b"</html>", cookie=True)
            if self.corrupt and self.corrupt[0] == path:
                value[self.corrupt[1]] = self.corrupt[2]
        return '$ ' + json.dumps(value) + '\n$ You are serial-admin\n'


class HttpBridgeTests(unittest.TestCase):
    credentials = {"username": "tester-private", "password": "Private!pässword4", "ap_password": "PrivateAP!123"}

    def test_body_integrity_and_complete_bounds(self):
        good = response(b"hello")
        self.assertEqual(validate_reply(good).text(), "hello")
        for changed in ({**good, "complete": False}, {**good, "bytes": 6}, {**good, "sha256": "0" * 64},
                        {**good, "body": "not base64"}, {**good, "bodyIncluded": False}, {**good, "bytes": 2_000_001}):
            with self.assertRaises(HttpBridgeError):
                validate_reply(changed)

    def test_bridge_parser_does_not_reflect_unsafe_error_payload(self):
        value = response(b"{}")
        self.assertEqual(parse_bridge('$ ' + json.dumps(value)), value)
        with self.assertRaisesRegex(HttpBridgeError, "invalid_error_response"):
            parse_bridge('{"schema":1,"bridge":"s3-http-v1","ok":false,"error":"private secret!"}')

    def test_full_auth_pages_commands_logout_and_no_payload_in_result(self):
        board = FakeConsole(self.credentials)
        callbacks = []
        with tempfile.TemporaryDirectory() as directory:
            result = run_probe_http(board, self.credentials, on_cycle=lambda index: callbacks.append((index, board.cookie)), run_root=directory)
            self.assertEqual(result["status"], "passed")
            self.assertTrue(result["verified"]["logout"])
            self.assertEqual(result["verified"]["htmlPages"], 5)
            self.assertEqual(callbacks, [(0, True)])
            self.assertFalse(board.cookie)
            output = Path(result["resultPath"])
            self.assertEqual(stat.S_IMODE(output.stat().st_mode), 0o600)
            rendered = output.read_text()
            for private in private_encodings(self.credentials):
                self.assertNotIn(private, rendered)
                self.assertNotIn(json.dumps(private)[1:-1], rendered)
            self.assertNotIn('"body":', rendered)
            self.assertNotIn('<html>', rendered)
            self.assertTrue(any(not row["bodyIncluded"] for row in result["requests"]))

    def test_incomplete_html_fails_and_clears_cookie_without_repair(self):
        board = FakeConsole(self.credentials, ("/settings", "htmlClose", False))
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(HttpBridgeError, "Incomplete HTML page"):
                run_probe_http(board, self.credentials, join=False, run_root=directory)
            self.assertEqual(board.commands[-1], "httpprobe clear")
            self.assertFalse(board.cookie)
            self.assertFalse(any(command.startswith("httpprobe join") for command in board.commands))
            paths = list(Path(directory).glob("*/results.json"))
            self.assertEqual(len(paths), 1)
            self.assertEqual(json.loads(paths[0].read_text())["status"], "failed")

    def test_login_requires_cookie_and_expected_redirect(self):
        board = FakeConsole(self.credentials, ("/login", "cookiePresent", False))
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(HttpBridgeError, "cookie login failed"):
                run_probe_http(board, self.credentials, run_root=directory)


if __name__ == "__main__":
    unittest.main()
