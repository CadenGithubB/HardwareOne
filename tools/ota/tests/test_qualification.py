from __future__ import annotations

import argparse
import json
import pathlib
import queue
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

from tools.ota import make_manifest, make_test_fixtures
from tools.ota.qualification.artifacts import load_verified_artifacts
from tools.ota.qualification.evidence import EvidenceRecorder, Redactor
from tools.ota.qualification.model import Checkpoint
from tools.ota.qualification.recovery_http import (
    NoHttpResponse,
    RecoveryClient,
    parse_http,
)
from tools.ota.qualification.scenarios import SCENARIOS, get_scenario
from tools.ota.qualification.serial_observer import SerialObserver
from tools.ota.qualification.transports import response_is_complete


REPOSITORY = pathlib.Path(__file__).resolve().parents[3]


class ModelAndScenarioTests(unittest.TestCase):
    def test_all_staged_contracts_are_unique_complete_and_reviewable(self) -> None:
        self.assertEqual(
            [scenario.case_id for scenario in SCENARIOS],
            [f"STG-{number:03d}" for number in range(1, 19)],
        )
        for scenario in SCENARIOS:
            with self.subTest(case=scenario.case_id):
                self.assertTrue(scenario.steps)
                self.assertTrue(scenario.required_result)
                self.assertEqual(scenario.execution, "ordinary-firmware")
                self.assertTrue(
                    scenario.destructive
                    or any(step.mutates_device for step in scenario.steps),
                    "every staged scenario should expose its mutation/risk",
                )
        self.assertEqual(get_scenario("stg-018").case_id, "STG-018")
        with self.assertRaises(KeyError):
            get_scenario("STG-999")

    def test_checkpoint_round_trip_rejects_bad_digest_and_sequences(self) -> None:
        checkpoint = Checkpoint(
            run_id="run-12345678",
            case_id="STG-012",
            board="feathers3",
            layout="hw1-f3-ota-v1",
            image_sha256="a" * 64,
            manifest_sha256="b" * 64,
            journal_sequence=4,
        )
        restored = Checkpoint.from_dict(checkpoint.as_dict())
        self.assertEqual(restored, checkpoint)

        bad = checkpoint.as_dict()
        bad["imageSha256"] = "short"
        with self.assertRaisesRegex(ValueError, "digest"):
            Checkpoint.from_dict(bad)
        bad = checkpoint.as_dict()
        bad["journalSequence"] = -1
        with self.assertRaisesRegex(ValueError, "sequence"):
            Checkpoint.from_dict(bad)


class EvidenceTests(unittest.TestCase):
    def test_recursive_redaction_and_restartable_checkpoint(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary) / "runs"
            recorder = EvidenceRecorder(
                root, run_id="run-12345678", secret_values=("secret-value",)
            )
            directory = recorder.create()
            recorder.write_json(
                "nested/result.json",
                {
                    "password": "secret-value",
                    "message": "Bearer abcDEF_123 and secret-value",
                    "nested": {"sessionCookie": "cookie-value"},
                },
            )
            recorder.append_event(
                "http.response",
                {"header": "Authorization: Basic YWRtaW46c2VjcmV0"},
            )
            checkpoint = Checkpoint(
                run_id=recorder.run_id,
                case_id="STG-001",
                board="feathers3",
                layout="hw1-f3-ota-v1",
                image_sha256="1" * 64,
                manifest_sha256="2" * 64,
            )
            recorder.write_checkpoint(checkpoint)
            combined = "\n".join(
                path.read_text(encoding="utf-8")
                for path in directory.rglob("*")
                if path.is_file()
            )
            self.assertNotIn("secret-value", combined)
            self.assertNotIn("cookie-value", combined)
            self.assertNotIn("YWRtaW46c2VjcmV0", combined)
            self.assertEqual(EvidenceRecorder.load_checkpoint(directory), checkpoint)
            self.assertEqual(directory.stat().st_mode & 0o777, 0o700)
            self.assertEqual(
                (directory / "checkpoint.json").stat().st_mode & 0o777, 0o600
            )

    def test_evidence_path_cannot_escape_run_directory(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            recorder = EvidenceRecorder(
                pathlib.Path(temporary), run_id="run-12345678"
            )
            recorder.create()
            with self.assertRaisesRegex(ValueError, "escapes"):
                recorder.write_json("../../outside.json", {})

    def test_redactor_does_not_hide_public_key_fingerprint(self) -> None:
        value = Redactor().value(
            {"publicKeyFingerprint": "abc", "privateKeyPath": "/secret/key"}
        )
        self.assertEqual(value["publicKeyFingerprint"], "abc")
        self.assertEqual(value["privateKeyPath"], "[REDACTED]")


class FakeByteTransport:
    def __init__(self, response: bytes):
        self.response = response
        self.requests: list[bytes] = []

    def exchange(self, _host: str, _port: int, request: bytes, _timeout: float) -> bytes:
        self.requests.append(request)
        return self.response


class RecoveryTransportTests(unittest.TestCase):
    def test_status_uses_basic_auth_without_putting_secret_in_a_process_argv(self) -> None:
        body = b'{"project":"hw1-updater"}'
        raw = (
            b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
            + f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
            + body
        )
        transport = FakeByteTransport(raw)
        client = RecoveryClient(transport, "recovery-secret")
        self.assertEqual(client.status()["project"], "hw1-updater")
        request = transport.requests[0]
        self.assertIn(b"Authorization: Basic ", request)
        self.assertNotIn(b"recovery-secret", request)

    def test_parser_rejects_missing_or_incomplete_http(self) -> None:
        with self.assertRaises(NoHttpResponse):
            parse_http(b"")
        with self.assertRaisesRegex(RuntimeError, "incomplete"):
            parse_http(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\na")

    def test_raw_response_completion_requires_satisfied_content_length(self) -> None:
        self.assertTrue(
            response_is_complete(
                b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc"
            )
        )
        self.assertFalse(
            response_is_complete(
                b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nabc"
            )
        )


class FakeSerial:
    def __init__(self, lines: list[bytes]):
        self.lines: queue.Queue[bytes] = queue.Queue()
        for line in lines:
            self.lines.put(line)
        self.closed = False

    def readline(self) -> bytes:
        try:
            return self.lines.get(timeout=0.02)
        except queue.Empty:
            return b""

    def close(self) -> None:
        self.closed = True


class SerialObserverTests(unittest.TestCase):
    def test_serial_capture_timestamps_and_matches_without_owning_reset_lines(self) -> None:
        fake = FakeSerial([b"booting\r\n", b"recovery ready\n"])
        calls: list[tuple[str, int, float]] = []

        def factory(port: str, baud: int, timeout: float) -> FakeSerial:
            calls.append((port, baud, timeout))
            return fake

        with tempfile.TemporaryDirectory() as temporary:
            output = pathlib.Path(temporary) / "serial.log"
            observer = SerialObserver("/dev/test-port", output, factory=factory)
            observer.start()
            self.assertEqual(observer.wait_for(r"recovery ready", 1.0), "recovery ready")
            observer.close()
            self.assertTrue(fake.closed)
            self.assertEqual(calls, [("/dev/test-port", 115200, 0.25)])
            log = output.read_text(encoding="utf-8")
            self.assertIn("booting", log)
            self.assertIn("recovery ready", log)


@unittest.skipUnless(shutil.which("openssl"), "openssl is required")
class FixtureAndCliTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = pathlib.Path(cls.temporary.name)
        cls.key = cls.root / "lab-key.pem"
        result = subprocess.run(
            [
                "openssl",
                "genpkey",
                "-algorithm",
                "RSA",
                "-pkeyopt",
                "rsa_keygen_bits:3072",
                "-out",
                str(cls.key),
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise RuntimeError(result.stderr)
        cls.image = cls.root / "fake-main.bin"
        image = bytearray(4096)
        descriptor = make_manifest.APP_DESC_OFFSET
        struct.pack_into("<I", image, descriptor, make_manifest.APP_DESC_MAGIC)
        image[descriptor + 16 : descriptor + 16 + len(b"9.9.9+f3o1")] = b"9.9.9+f3o1"
        image[descriptor + 48 : descriptor + 48 + len(b"hardwareone-idf")] = b"hardwareone-idf"
        for index in range(descriptor + 256, len(image)):
            image[index] = index % 251
        cls.image.write_bytes(image)
        cls.output = cls.root / "fixtures"
        make_test_fixtures.generate(
            argparse.Namespace(
                image=cls.image,
                lab_key=cls.key,
                output=cls.output,
                board="feathers3",
                acknowledge_lab_key=True,
            )
        )

    @classmethod
    def tearDownClass(cls) -> None:
        cls.temporary.cleanup()

    def test_fixture_index_is_complete_reproducible_and_contains_no_key_path(self) -> None:
        index_path = self.output / "fixture-index.json"
        index = json.loads(index_path.read_text(encoding="utf-8"))
        self.assertEqual(index["format"], make_test_fixtures.INDEX_FORMAT)
        self.assertEqual(len(index["fixtures"]), 16)
        self.assertEqual(
            {item["id"] for item in index["fixtures"]},
            {
                "valid-control",
                "candidate-only",
                "manifest-only",
                "truncated-image",
                "truncated-manifest",
                "same-size-digest-mismatch",
                "invalid-manifest-signature",
                "native-signature-invalid",
                "wrong-board",
                "wrong-layout",
                "unsupported-schema",
                "min-updater-too-new",
                "wrong-size",
                "wrong-digest",
                "malformed-envelope",
                "oversized-envelope",
            },
        )
        self.assertNotIn(str(self.key), index_path.read_text(encoding="utf-8"))
        self.assertEqual(
            (self.output / "manifests/oversized-envelope.json").stat().st_size,
            2049,
        )
        with self.assertRaisesRegex(ValueError, "already exists"):
            make_test_fixtures.generate(
                argparse.Namespace(
                    image=self.image,
                    lab_key=self.key,
                    output=self.output,
                    board="feathers3",
                    acknowledge_lab_key=True,
                )
            )

    def test_native_invalid_fixture_has_a_valid_matching_detached_manifest(self) -> None:
        identity = load_verified_artifacts(
            self.output / "images/native-signature-invalid.bin",
            self.output / "manifests/native-signature-invalid.json",
            self.output / "lab-public-key.pem",
            expected_board="feathers3",
        )
        self.assertEqual(identity.version, "9.9.9+f3o1")

    def test_cli_preflight_init_and_resume_are_clean_json_and_non_destructive(self) -> None:
        common = [
            "--board",
            "feathers3",
            "--image",
            str(self.output / "images/good.bin"),
            "--manifest",
            str(self.output / "manifests/valid-control.json"),
            "--public-key",
            str(self.output / "lab-public-key.pem"),
        ]
        preflight = subprocess.run(
            [
                sys.executable,
                "tools/ota/hardware_qualification.py",
                "preflight",
                *common,
                "--json",
            ],
            cwd=REPOSITORY,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(preflight.returncode, 0, preflight.stderr)
        report = json.loads(preflight.stdout)
        self.assertTrue(report["artifactReady"])
        self.assertFalse(report["readyForDestructiveRun"])
        self.assertEqual(report["checks"]["pairAudit"]["status"], "SKIP")

        runs = self.root / "runs"
        initialize = subprocess.run(
            [
                sys.executable,
                "tools/ota/hardware_qualification.py",
                "init-run",
                *common,
                "--case",
                "STG-012",
                "--output",
                str(runs),
                "--disposable-device",
                "--json",
            ],
            cwd=REPOSITORY,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(initialize.returncode, 0, initialize.stderr)
        initialized = json.loads(initialize.stdout)
        run_directory = pathlib.Path(initialized["runDirectory"])
        summary = json.loads(
            (run_directory / "summary.json").read_text(encoding="utf-8")
        )
        self.assertTrue(summary["dryRunOnly"])
        self.assertFalse(summary["destructiveExecutorPresent"])

        resume = subprocess.run(
            [
                sys.executable,
                "tools/ota/hardware_qualification.py",
                "resume",
                "--run",
                str(run_directory),
                "--json",
            ],
            cwd=REPOSITORY,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(resume.returncode, 0, resume.stderr)
        self.assertFalse(json.loads(resume.stdout)["executorPresent"])


if __name__ == "__main__":
    unittest.main()
