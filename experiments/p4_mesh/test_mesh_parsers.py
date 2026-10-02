"""Offline protocol-evidence tests: python3 experiments/p4_mesh/test_mesh_parsers.py."""

import base64
import json
from pathlib import Path
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest

from test_mesh import (CheckFailed, MeshRunner, check_session_pair, command_token,
                       decode_file_chunk, deterministic_text, find_object,
                       history_rows, parse_args, reconstruct_text, select_session)
from test_mesh import parse_debug_state, select_registered_peer


MAC = "02:48:57:31:01:a8"


def text_rows(payload, msg_id=71):
    pieces = [payload] if len(payload) <= 202 else [payload[i:i + 200] for i in range(0, len(payload), 200)]
    return [{"seq": index + 1, "reqId": msg_id, "piece": index + 1, "of": len(pieces),
             "mac": MAC.upper(), "msg": piece, "enc": True, "type": 0, "sent": False}
            for index, piece in enumerate(pieces)]


class MeshParserTests(unittest.TestCase):
    def test_actual_text_boundaries_and_exact_reassembly(self):
        for length, count in ((1, 1), (201, 1), (202, 1), (203, 2), (400, 2), (401, 3), (1024, 6)):
            with self.subTest(length=length):
                payload = deterministic_text("offline-test", length)
                evidence = reconstruct_text(list(reversed(text_rows(payload))), MAC, 71, payload)
                self.assertEqual(evidence["pieces"], count)
                self.assertEqual(evidence["bytes"], length)
                self.assertTrue(evidence["encrypted"])

    def test_other_message_peer_and_sent_rows_are_excluded(self):
        payload = "test"
        rows = text_rows(payload)
        rows += [{**rows[0], "reqId": 72}, {**rows[0], "sent": True},
                 {**rows[0], "mac": "11:22:33:44:55:66"}, {**rows[0], "type": 11}]
        self.assertIsNotNone(reconstruct_text(rows, MAC, 71, payload))

    def test_missing_fragment_is_incomplete(self):
        payload = "x" * 401
        self.assertIsNone(reconstruct_text(text_rows(payload)[:2], MAC, 71, payload))

    def test_duplicate_plaintext_malformed_and_corruption_fail(self):
        payload = "x" * 401
        for alteration in ({"enc": False}, {"piece": 0}, {"of": 4}, {"msg": "corrupt"}):
            with self.subTest(alteration=alteration):
                rows = text_rows(payload)
                rows[0] = {**rows[0], **alteration}
                with self.assertRaises(CheckFailed):
                    reconstruct_text(rows, MAC, 71, payload)
        rows = text_rows(payload)
        with self.assertRaises(CheckFailed):
            reconstruct_text(rows + [rows[0]], MAC, 71, payload)

    def test_history_pages_require_strict_sequence_and_requested_peer(self):
        rows = text_rows("x" * 401)
        actual, cursor = history_rows({"schema": 1, "messages": rows}, 0, MAC)
        self.assertEqual(cursor, 3)
        self.assertEqual(actual, rows)
        for bad_rows in (list(reversed(rows)), rows + [rows[0]], [{**rows[0], "mac": "11:22:33:44:55:66"}]):
            with self.assertRaises(CheckFailed):
                history_rows({"schema": 1, "messages": bad_rows}, 0, MAC)

    def test_session_requires_active_matching_identity_and_opposite_directions(self):
        row = {"mac": MAC.upper(), "state": "ACTIVE", "dir": "A", "sessionId": 31,
               "meshId": 0, "ageMs": 42, "txSeq": 2, "rxHwm": 1}
        self.assertEqual(select_session({"schema": 1, "sessions": [row]}, MAC), row)
        self.assertIsNone(select_session({"schema": 1, "sessions": [{**row, "state": "REKEY"}]}, MAC))
        check_session_pair(row, {**row, "dir": "B"})
        for other in ({**row, "dir": "A"}, {**row, "dir": "B", "sessionId": 32}):
            with self.assertRaises(CheckFailed):
                check_session_pair(row, other)

    def test_identity_presence_cannot_substitute_for_local_pair_registry(self):
        self.assertIsNone(select_registered_peer({"schema": 1, "count": 0, "devices": []}, MAC))
        row = {"mac": MAC, "name": "peer", "encrypted": True, "meshId": 0}
        self.assertEqual(select_registered_peer({"schema": 1, "count": 1, "devices": [row]}, MAC), row)
        with self.assertRaises(CheckFailed):
            select_registered_peer({"schema": 1, "count": 1, "devices": [{**row, "encrypted": False}]}, MAC)
        with self.assertRaises(CheckFailed):
            select_registered_peer({"schema": 1, "count": 1, "devices": [row]}, MAC, mesh_id=2)

    def test_session_is_bound_to_local_requested_mesh_slot(self):
        row = {"mac": MAC, "state": "ACTIVE", "dir": "B", "sessionId": 31,
               "meshId": 2, "ageMs": 42, "txSeq": 2, "rxHwm": 1}
        self.assertEqual(select_session({"schema": 1, "sessions": [row]}, MAC, mesh_id=2), row)
        with self.assertRaises(CheckFailed):
            select_session({"schema": 1, "sessions": [row]}, MAC, mesh_id=0)

    def test_health_check_retains_split_panic_prefix(self):
        stream = ["Guru Medi"]
        board = SimpleNamespace(marker=lambda: len(stream[0]), read_since=lambda mark: stream[0][mark:])
        runner = object.__new__(MeshRunner)
        runner.boards = {"p4": board}
        runner.monitor_marks = {"p4": 0}
        runner.monitor_tails = {"p4": ""}
        runner.check_health()
        stream[0] += "tation Error"
        with self.assertRaises(CheckFailed):
            runner.check_health()

    def test_debug_snapshot_uses_actual_runtime_mask(self):
        flags = "Debug flags: 0x0000000000000000:0000000000000000:0000000000000000:0000000100000000"
        self.assertEqual(parse_debug_state(flags, "Current log level: warn (1) (0=error, 1=warn, 2=info, 3=debug)"),
                         {"core": True, "level": 1, "levelName": "warn"})

    def test_rekey_debug_settings_restore_even_on_test_failure(self):
        flags = "Debug flags: 0x0000000000000000:0000000000000000:0000000000000000:0000000000000000"
        board = SimpleNamespace(marker=lambda: 0, read_until=lambda *args, **kwargs: flags)
        runner = object.__new__(MeshRunner)
        runner.boards = {"p4": board}
        commands = []
        def command(key, text):
            commands.append(text)
            return "Current log level: warn (1)" if text == "loglevel" else "OK"
        runner.command = command
        with self.assertRaisesRegex(CheckFailed, "deliberate"):
            with runner.rekey_logging():
                raise CheckFailed("deliberate")
        self.assertEqual(commands, ["debugflags", "loglevel", "debugespnowcore 1 temp", "loglevel info",
                                    "loglevel warn", "debugespnowcore 0 temp"])

    def test_file_chunk_validates_binary_and_envelope(self):
        data = bytes(range(256)) * 2
        document = {"success": True, "path": "/fixture.bin", "size": 1024, "offset": 0,
                    "len": 512, "eof": False, "enc": "b64", "data": base64.b64encode(data).decode()}
        self.assertEqual(decode_file_chunk(document, 0, 1024, "/fixture.bin"), data)
        for changes in ({"size": 512}, {"offset": 512}, {"len": 511}, {"eof": True},
                        {"data": "invalid!"}, {"path": "/other.bin"}, {"success": False}):
            with self.subTest(changes=changes), self.assertRaises(CheckFailed):
                decode_file_chunk({**document, **changes}, 0, 1024, "/fixture.bin")

    def test_json_selection_and_schema_reject_ambiguous_response(self):
        self.assertEqual(find_object('[serial] {"schema":1,"ok":true,"msgId":1}\n$ ', "ok")["msgId"], 1)
        with self.assertRaises(CheckFailed):
            find_object('{"schema":2,"ok":true}', "ok")
        with self.assertRaises(CheckFailed):
            find_object('{"schema":1,"ok":true}\n{"schema":1,"ok":false}', "ok")

    def test_cli_defaults_preserve_settings_and_pairing(self):
        args = parse_args([])
        self.assertFalse(args.configure)
        self.assertEqual(args.pair, "preserve")
        self.assertFalse(args.rekey)
        self.assertFalse(args.reopen)
        self.assertEqual(args.startup_delay, 8)
        self.assertEqual(args.initiator, "p4")
        self.assertEqual(parse_args(["--initiator", "s3"]).initiator, "s3")

    def test_discovery_request_accept_and_marker_follow_selected_initiator(self):
        for initiator in ("p4", "s3"):
            with self.subTest(initiator=initiator):
                calls, waits = [], []
                runner = object.__new__(MeshRunner)
                runner.args = SimpleNamespace(pair="discovery", initiator=initiator)
                runner.config = {"p4": {"mac": MAC}, "s3": {"mac": "02:48:57:31:00:d0"}}
                runner.boards = {
                    key: SimpleNamespace(marker=lambda key=key: 14 if key == "p4" else 33,
                                         read_until=lambda pattern, key=key, **kwargs: waits.append((key, kwargs)))
                    for key in ("p4", "s3")}
                runner.known_peer = lambda *args: False
                runner.registries = lambda: {"p4": None, "s3": None}
                runner.pair_ready = lambda: {"senderReadyProbes": ["verified"]}
                def command(key, text, **kwargs):
                    calls.append((key, text))
                    if text == "espnowdiscovered":
                        other = "s3" if key == "p4" else "p4"
                        return runner.config[other]["mac"]
                    return "Accepted" if text.startswith("espnowaccept") else "OK"
                runner.command = command
                evidence = runner.pair()
                accepter = "s3" if initiator == "p4" else "p4"
                self.assertIn((initiator, f"espnowpairrequest {runner.config[accepter]['mac']}"), calls)
                self.assertIn((accepter, f"espnowaccept {runner.config[initiator]['mac']}"), calls)
                self.assertEqual(waits, [(accepter, {"since": 14 if accepter == "p4" else 33, "timeout": 15})])
                self.assertEqual(evidence["initiator"], initiator)
                self.assertEqual(evidence["accepter"], accepter)

    def test_failure_statistics_retain_healthy_board_and_redact_errors(self):
        runner = object.__new__(MeshRunner)
        def broken(command, timeout):
            self.assertEqual(timeout, 5)
            raise RuntimeError("private-error-value")
        runner.boards = {
            "p4": SimpleNamespace(command=lambda command, timeout: '{"schema":1,"rxRingDrops":4}'),
            "s3": SimpleNamespace(command=broken)}
        runner.results = {}
        runner.safe = lambda text: text.replace("private-error-value", "[REDACTED]")
        runner.save = lambda: None
        runner.capture_failure_stats()
        self.assertEqual(runner.results["statistics"]["end"]["p4"]["rxRingDrops"], 4)
        self.assertEqual(runner.results["endStatisticsCapture"]["errors"], {"s3": "RuntimeError: [REDACTED]"})

    def test_failure_statistics_deadline_bounds_blocked_serial(self):
        release = threading.Event()
        runner = object.__new__(MeshRunner)
        runner.boards = {"p4": SimpleNamespace(command=lambda *args, **kwargs: release.wait(2))}
        runner.results = {}
        runner.safe = lambda text: text
        runner.save = lambda: None
        started = time.monotonic()
        try:
            runner.capture_failure_stats(timeout=0.03)
            self.assertLess(time.monotonic() - started, 0.5)
            self.assertIn("deadline", runner.results["endStatisticsCapture"]["errors"]["p4"])
        finally:
            release.set()

    def test_failure_capture_cannot_replace_original_test_error(self):
        runner = object.__new__(MeshRunner)
        original = CheckFailed("original test failure")
        def fail_run():
            raise original
        def fail_capture():
            raise RuntimeError("capture failed")
        runner.run = fail_run
        runner.capture_failure_stats = fail_capture
        runner.results = {}
        with self.assertRaises(CheckFailed) as caught:
            runner.run_with_failure_capture(0)
        self.assertIs(caught.exception, original)
        self.assertIn("endStatisticsCaptureError", runner.results)

    def test_tokens_do_not_accept_command_injection_or_unparseable_quotes(self):
        self.assertEqual(command_token("space allowed"), '"space allowed"')
        for value in ('contains"quote', "line\ncommand", "\rrestart", ""):
            with self.assertRaises(CheckFailed):
                command_token(value)

    def test_json_artifact_redaction_happens_before_unicode_escaping(self):
        private_value = "private_\u00e9_value"
        fake = SimpleNamespace(redact=lambda text: text.replace(private_value, "[REDACTED]"))
        args = SimpleNamespace(configure=False, pair="preserve", initiator="p4", rekey=False, reopen=False)
        with tempfile.TemporaryDirectory() as directory:
            runner = MeshRunner({"p4": fake, "s3": fake}, {}, {}, Path(directory), "offline", args)
            runner.results["error"] = private_value
            runner.save()
            document = json.loads((Path(directory) / "results.json").read_text())
            self.assertEqual(document["error"], "[REDACTED]")


if __name__ == "__main__":
    unittest.main()
