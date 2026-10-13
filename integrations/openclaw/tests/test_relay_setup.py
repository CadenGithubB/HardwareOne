"""Relay preparation tests; disposable files and loopback TLS only, no deployment."""

from contextlib import redirect_stderr, redirect_stdout
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import plistlib
import shutil
import socket
import ssl
import stat
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock


RELAY_DIR = Path(__file__).resolve().parents[1] / "relay"
SCRIPT = RELAY_DIR / "setup_relay.py"
SPEC = importlib.util.spec_from_file_location("hardwareone_relay_setup", SCRIPT)
setup = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = setup
sys.path.insert(0, str(RELAY_DIR))
try:
    SPEC.loader.exec_module(setup)
finally:
    sys.path.pop(0)

# Clearly fake fixture credential, never a real OpenClaw gateway token.
GATEWAY_TOKEN = "setup-test-only-gateway-" + "g" * 40


def file_snapshot(root):
    return {str(path.relative_to(root)): (stat.S_IMODE(path.stat().st_mode),
                                         hashlib.sha256(path.read_bytes()).hexdigest())
            for path in root.rglob("*") if path.is_file()}


@unittest.skipUnless(shutil.which("openssl"), "openssl is required for generated certificate tests")
class RelaySetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="hw1-relay-setup-test-")
        # macOS tempfile paths can begin /var, a symlink to /private/var; setup
        # intentionally requires a canonical parent before publishing files.
        cls.root = Path(cls.tmp.name).resolve()
        cls.token_file = cls.root / "fake-existing-gateway-token"
        cls.token_file.write_text(GATEWAY_TOKEN + "\n")
        cls.token_file.chmod(0o600)
        cls.runtime = cls.root / "prepared"
        cls.fake_home = cls.root / "operator-home"
        (cls.fake_home / ".openclaw").mkdir(parents=True)
        cls.host_config = cls.fake_home / ".openclaw" / "openclaw.json"
        cls.host_config.write_text('{"sentinel":"existing meticulous configuration"}\n')
        cls.environment = dict(os.environ, HOME=str(cls.fake_home))
        cls.prepared = subprocess.run(
            [sys.executable, str(SCRIPT), "--bind", "127.0.0.1", "--output", str(cls.runtime),
             "--gateway-token-file", str(cls.token_file)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=cls.environment, check=False, timeout=15)
        if cls.prepared.returncode:
            raise AssertionError("Preparation failed: " + cls.prepared.stderr.decode())

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def arguments(self, destination, **changes):
        values = {"bind": "127.0.0.1", "output": str(destination),
                  "gateway-token-file": str(self.token_file)}
        values.update(changes)
        return [item for key, value in values.items() for item in ("--" + key, str(value))]

    def execute(self, destination, **changes):
        return subprocess.run([sys.executable, str(SCRIPT), *self.arguments(destination, **changes)],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=self.environment,
                              check=False, timeout=15)

    def assert_no_staging_directories(self):
        self.assertEqual(list(self.root.glob(".hw1-relay-*")), [])

    def test_runtime_permissions_and_distinct_device_key(self):
        self.assertEqual(stat.S_IMODE(self.runtime.stat().st_mode), 0o700)
        expected = {"config.json", "relay.py", "launchagent.plist", "ca-key.pem", "ca-cert.pem",
                    "relay-key.pem", "relay-cert.pem", "device-token.txt", "gateway-token.txt"}
        self.assertEqual({path.name for path in self.runtime.iterdir()}, expected)
        for path in self.runtime.iterdir():
            with self.subTest(path=path.name):
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
        gateway = (self.runtime / "gateway-token.txt").read_text().strip()
        device = (self.runtime / "device-token.txt").read_text().strip()
        self.assertEqual(gateway, GATEWAY_TOKEN)
        self.assertNotEqual(gateway, device)
        self.assertGreaterEqual(len(device), 32)
        self.assertRegex(device, r"^[a-zA-Z0-9_-]+$")
        self.assert_no_staging_directories()

    def test_atomic_publish_rewrites_paths_and_final_config_validates(self):
        config = json.loads((self.runtime / "config.json").read_text())
        self.assertEqual(config["bind_host"], "127.0.0.1")
        self.assertEqual(config["upstream_port"], 18789)
        for key in ("tls_cert_file", "tls_key_file", "device_token_file", "upstream_token_file"):
            self.assertEqual(Path(config[key]).parent, self.runtime)
            self.assertTrue(Path(config[key]).is_file())
        result = subprocess.run([sys.executable, str(self.runtime / "relay.py"), "--config",
                                 str(self.runtime / "config.json"), "--check-config"],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(b"no network connection made", result.stdout)

    def test_ca_trust_and_exact_ip_certificate_work_in_a_real_tls_handshake(self):
        server_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        server_context.load_cert_chain(str(self.runtime / "relay-cert.pem"), str(self.runtime / "relay-key.pem"))
        client_context = ssl.create_default_context(cafile=str(self.runtime / "ca-cert.pem"))
        outcomes = []
        listener = socket.socket()
        self.addCleanup(listener.close)
        listener.bind(("127.0.0.1", 0))
        listener.listen(3)
        listener.settimeout(3)

        def serve():
            for _ in range(3):
                raw, _address = listener.accept()
                raw.settimeout(2)
                try:
                    with server_context.wrap_socket(raw, server_side=True) as secure:
                        secure.sendall(b"verified-ip")
                        outcomes.append("accepted")
                except ssl.SSLError:
                    raw.close()
                    outcomes.append("refused")

        worker = threading.Thread(target=serve, daemon=True)
        worker.start()
        try:
            with socket.create_connection(listener.getsockname(), timeout=2) as raw:
                with client_context.wrap_socket(raw, server_hostname="127.0.0.1") as secure:
                    self.assertEqual(secure.recv(32), b"verified-ip")
            with socket.create_connection(listener.getsockname(), timeout=2) as raw:
                with self.assertRaises(ssl.SSLCertVerificationError):
                    client_context.wrap_socket(raw, server_hostname="127.0.0.2")
            untrusted = ssl.create_default_context()
            with socket.create_connection(listener.getsockname(), timeout=2) as raw:
                with self.assertRaises(ssl.SSLCertVerificationError):
                    untrusted.wrap_socket(raw, server_hostname="127.0.0.1")
            worker.join(timeout=3)
            self.assertFalse(worker.is_alive())
        finally:
            listener.close()
        self.assertEqual(outcomes, ["accepted", "refused", "refused"])

    def test_success_output_is_public_and_existing_host_configuration_unchanged(self):
        output = self.prepared.stdout + self.prepared.stderr
        device = (self.runtime / "device-token.txt").read_bytes().strip()
        for private in (GATEWAY_TOKEN.encode(), device, b"BEGIN PRIVATE KEY", b"BEGIN RSA PRIVATE KEY"):
            self.assertNotIn(private, output)
        self.assertIn(b"https://127.0.0.1:18790/v1", output)
        self.assertIn(b"No service has been started or installed", output)
        self.assertEqual(self.host_config.read_text(), '{"sentinel":"existing meticulous configuration"}\n')
        self.assertFalse((self.fake_home / "Library" / "LaunchAgents").exists())
        with open(self.runtime / "launchagent.plist", "rb") as source:
            launch = plistlib.load(source)
        self.assertEqual(launch["ProgramArguments"], [sys.executable, str(self.runtime / "relay.py"),
                                                     "--config", str(self.runtime / "config.json")])
        self.assertEqual(launch["StandardOutPath"], str(self.runtime / "relay.log"))

    def test_existing_output_and_dangling_symlink_are_refused_unchanged(self):
        before = file_snapshot(self.runtime)
        result = self.execute(self.runtime)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"already exists", result.stderr)
        self.assertEqual(file_snapshot(self.runtime), before)
        link = self.root / "dangling-output"
        link.symlink_to(self.root / "absent-target")
        result = self.execute(link)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(link.is_symlink())
        self.assertFalse((self.root / "absent-target").exists())
        self.assert_no_staging_directories()

    def test_bad_network_and_device_parameters_never_publish(self):
        for index, changes in enumerate(({"bind": "0.0.0.0"}, {"bind": "localhost"},
                                         {"bind": "224.0.0.1"}, {"bind": "::1"},
                                         {"port": 0}, {"upstream-port": 65536},
                                         {"device-id": "../../different-agent"})):
            with self.subTest(changes=changes):
                destination = self.root / ("invalid-parameters-" + str(index))
                result = self.execute(destination, **changes)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(destination.exists())
                self.assertNotIn(GATEWAY_TOKEN.encode(), result.stdout + result.stderr)
                self.assert_no_staging_directories()

    def test_bad_credential_files_never_publish_or_echo_their_content(self):
        credential = self.root / "invalid-credential"
        for index, value in enumerate(("", "private-token-with spaces", "x" * 1025, "\u2603" * 30)):
            credential.write_text(value)
            credential.chmod(0o600)
            destination = self.root / ("invalid-credential-" + str(index))
            with self.subTest(index=index):
                result = self.execute(destination, **{"gateway-token-file": credential})
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(destination.exists())
                if value:
                    self.assertNotIn(value.encode(), result.stdout + result.stderr)
                self.assert_no_staging_directories()
        credential.write_text(GATEWAY_TOKEN)
        credential.chmod(0o644)
        destination = self.root / "loose-credential"
        self.assertNotEqual(self.execute(destination, **{"gateway-token-file": credential}).returncode, 0)
        self.assertFalse(destination.exists())

    def test_interactive_path_uses_masked_prompt_without_secret_output(self):
        destination = self.root / "interactive-prepared"
        output = io.StringIO()
        with mock.patch.object(setup.sys.stdin, "isatty", return_value=True):
            with mock.patch.object(setup.getpass, "getpass", return_value=GATEWAY_TOKEN) as prompt:
                with redirect_stdout(output), redirect_stderr(output):
                    result = setup.main(["--bind", "127.0.0.1", "--output", str(destination)])
        self.assertEqual(result, 0)
        prompt.assert_called_once_with("Existing OpenClaw gateway token (hidden): ")
        self.assertNotIn(GATEWAY_TOKEN, output.getvalue())
        self.assertEqual((destination / "gateway-token.txt").read_text().strip(), GATEWAY_TOKEN)

    def test_noninteractive_prompt_refuses_without_reading_or_echoing_piped_secret(self):
        destination = self.root / "piped-credential-refused"
        result = subprocess.run([sys.executable, str(SCRIPT), "--bind", "127.0.0.1", "--output", str(destination)],
                                input=(GATEWAY_TOKEN + "\n").encode(), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=self.environment, check=False, timeout=3)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Run interactively", result.stderr)
        self.assertNotIn(GATEWAY_TOKEN.encode(), result.stdout + result.stderr)
        self.assertFalse(destination.exists())
        self.assert_no_staging_directories()

    def test_openssl_and_publish_failure_clean_staging_and_restore_umask(self):
        for name, patch in (("openssl", mock.patch.object(setup, "openssl", side_effect=ValueError("fixture failure"))),
                            ("rename", mock.patch.object(setup.os, "rename", side_effect=OSError("fixture failure")))):
            destination = self.root / ("failed-" + name)
            old_mask = os.umask(0o027)
            try:
                with self.subTest(name=name), patch, redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
                    with self.assertRaises((ValueError, OSError)):
                        setup.main(self.arguments(destination))
                restored = os.umask(0o027)
                self.assertEqual(restored, 0o027)
            finally:
                os.umask(old_mask)
            self.assertFalse(destination.exists())
            self.assert_no_staging_directories()

    def test_output_appearing_during_preparation_is_preserved(self):
        destination = self.root / "concurrent-output"
        original_openssl = setup.openssl

        def create_output(*args):
            if not destination.exists():
                destination.mkdir()
                (destination / "existing-data").write_text("preserve me")
            return original_openssl(*args)

        with mock.patch.object(setup, "openssl", side_effect=create_output):
            with self.assertRaises(FileExistsError):
                setup.main(self.arguments(destination))
        self.assertEqual((destination / "existing-data").read_text(), "preserve me")
        self.assertEqual({path.name for path in destination.iterdir()}, {"existing-data"})
        self.assert_no_staging_directories()


if __name__ == "__main__":
    unittest.main()
