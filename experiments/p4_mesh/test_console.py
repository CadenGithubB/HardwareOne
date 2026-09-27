"""Offline transport/parser checks: python3 experiments/p4_mesh/test_console.py."""

import json
from pathlib import Path
import tempfile
import threading
import time
import unittest

from console import ConsoleTimeout, MeshConsole, PROMPT, extract_json_objects


class MockSerial:
    def __init__(self, **kwargs):
        self.port = kwargs["port"]
        self.timeout = kwargs["timeout"]
        self.dtr = self.rts = True
        self.closed = False
        self.open_state = None
        self.writes = []
        self.on_write = None
        self.pending = bytearray()
        self.condition = threading.Condition()

    def open(self):
        self.open_state = (self.port, self.dtr, self.rts)

    @property
    def in_waiting(self):
        with self.condition:
            return len(self.pending)

    def feed(self, data):
        with self.condition:
            self.pending.extend(data)
            self.condition.notify_all()

    def read(self, count):
        with self.condition:
            if not self.pending and not self.closed:
                self.condition.wait(self.timeout)
            data = bytes(self.pending[:count])
            del self.pending[:count]
            return data

    def write(self, data):
        self.writes.append((time.monotonic(), bytes(data)))
        if self.on_write:
            self.on_write(bytes(data))
        return len(data)

    def cancel_read(self):
        with self.condition:
            self.condition.notify_all()

    def close(self):
        with self.condition:
            self.closed = True
            self.condition.notify_all()


class JsonTests(unittest.TestCase):
    def test_nested_objects_and_quoted_braces(self):
        expected = {"schema": 1, "messages": [{"msg": 'braces } { and "quotes"', "enc": True}]}
        text = "boot log\n[serial user@local] " + json.dumps(expected) + '\n$ \n{"ok":true}\n'
        self.assertEqual(extract_json_objects(text), [expected, {"ok": True}])

    def test_repeated_prefix_fragments_inside_strings(self):
        expected = {"messages": [{"msg": "a long payload split into fragments", "piece": 2, "of": 3}]}
        encoded = json.dumps(expected)
        split = encoded.index("payload") + 3
        text = "\x1b[32m[serial user@local] " + encoded[:split] + "\r\n"
        text += "[serial user@local] " + encoded[split:] + "\x1b[0m\r\n$ "
        self.assertEqual(extract_json_objects(text), [expected])

    def test_partial_outer_is_not_a_complete_nested_response(self):
        self.assertEqual(extract_json_objects('{"messages":[{"enc":true}]'), [])


class ConsoleTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.mock = None

    def board(self, **kwargs):
        def factory(**serial_kwargs):
            self.mock = MockSerial(**serial_kwargs)
            return self.mock
        board = MeshConsole("MOCK_ONLY", Path(self.temp.name) / "console.log",
                            serial_factory=factory, **kwargs)
        self.addCleanup(board.close)
        return board

    def test_open_does_not_toggle_reset_lines(self):
        board = self.board()
        self.assertEqual(self.mock.open_state, ("MOCK_ONLY", False, False))
        self.assertEqual(board.log_path.stat().st_mode & 0o777, 0o600)

    def test_new_non_newline_prompt_not_existing_prompt_or_audit_line(self):
        board = self.board()
        self.mock.feed(b"$ ")
        board.read_until(PROMPT, since=0)
        def respond(data):
            if data.endswith(b"\n"):
                self.mock.feed(b"[serial] $ whoami\n")
                threading.Timer(0.05, lambda: self.mock.feed(b"You are milestone (admin)\n$ ")).start()
        self.mock.on_write = respond
        response = board.command("whoami")
        self.assertIn("You are milestone (admin)", response)
        self.assertTrue(response.endswith("$ "))

    def test_long_writes_are_paced_and_bounded(self):
        board = self.board()
        mark = board.send_line("x" * 150)
        self.assertEqual(mark, 0)
        self.assertEqual([len(data) for _, data in self.mock.writes], [64, 64, 23])
        self.assertEqual(b"".join(data for _, data in self.mock.writes), b"x" * 150 + b"\n")
        stamps = [stamp for stamp, _ in self.mock.writes]
        self.assertTrue(all(b - a >= 0.018 for a, b in zip(stamps, stamps[1:])))

    def test_prompt_followed_immediately_by_async_log(self):
        board = self.board()
        self.mock.on_write = lambda data: self.mock.feed(
            b"Pair request sent\r\n$ [4563] [CMD] milestone@serial: espnowpairmode 120 -> OK\r\n")
        response = board.command("espnowpairmode 120", timeout=0.2)
        self.assertIn("Pair request sent", response)
        self.assertIn("$ [4563]", response)

    def test_hardwareone_barrier_ignores_stale_prompt_before_json_response(self):
        board = self.board(completion="hardwareone")
        def respond(data):
            if data == b"espnowstatus json\n":
                self.mock.feed(b"$ ")  # old command's prompt arrives late
                threading.Timer(0.03, lambda: self.mock.feed(b'{"schema":1,"initialized":true}\r\n$ ')).start()
            elif data == b"whoami\n":
                threading.Timer(0.06, lambda: self.mock.feed(b"You are milestone (admin)\r\n$ [4563] [CMD] status -> OK\r\n")).start()
        self.mock.on_write = respond
        response = board.command("espnowstatus json", timeout=0.5)
        self.assertEqual(extract_json_objects(response), [{"schema": 1, "initialized": True}])
        self.assertIn("You are milestone (admin)", response)
        self.assertEqual([data for _, data in self.mock.writes], [b"espnowstatus json\n", b"whoami\n"])

    def test_hardwareone_whoami_requires_its_result_and_the_barrier_result(self):
        board = self.board(completion="hardwareone")
        count = [0]
        def respond(data):
            self.assertEqual(data, b"whoami\n")
            count[0] += 1
            if count[0] == 1:
                self.mock.feed(b"$ You are milestone (admin)\r\n$ ")
            else:
                threading.Timer(0.05, lambda: self.mock.feed(b"You are milestone (admin)\r\n$ ")).start()
        self.mock.on_write = respond
        response = board.command("whoami", timeout=0.3)
        self.assertEqual(response.count("You are milestone (admin)"), 2)

    def test_hardwareone_barrier_requires_complete_identity_line(self):
        board = self.board(completion="hardwareone")
        def respond(data):
            if data == b"whoami\n":
                self.mock.feed(b"$ You are milestone")
                threading.Timer(0.05, lambda: self.mock.feed(b" (admin)\r\n$ ")).start()
        self.mock.on_write = respond
        response = board.command("espnowchannel", timeout=0.3)
        self.assertIn("You are milestone (admin)\r\n", response)

    def test_secret_split_between_reads_never_reaches_log_or_callback(self):
        lines = []
        secret = "PRIVATE_test_password"
        board = self.board(secrets=[secret], on_log=lines.append)
        self.mock.feed(b"echo PRIVATE_test_")
        board.read_until("PRIVATE_test_", since=0)
        self.assertEqual(lines, [])  # do not log an incomplete credential
        self.mock.feed(b"password\nnext line\n")
        board.read_until("next line", since=0)
        board.send_line("login milestone " + secret)
        board.close()
        log = board.log_path.read_text()
        self.assertNotIn(secret, log)
        self.assertNotIn("PRIVATE_test_", log)
        self.assertIn("[REDACTED]", log)
        self.assertNotIn(secret, "\n".join(lines))
        self.assertIn(secret, board.read_since(0))  # raw only in memory

    def test_timeout_does_not_include_raw_response_in_exception_text(self):
        board = self.board(secrets=["private_value"])
        self.mock.feed(b"private_value")
        with self.assertRaises(ConsoleTimeout) as result:
            board.read_until("never", since=0, timeout=0.03)
        self.assertEqual(result.exception.output, "private_value")
        self.assertNotIn("private_value", str(result.exception))
        board.close()
        self.assertNotIn("private_value", board.log_path.read_text())

    def test_commands_are_serialized_through_response(self):
        board = self.board()
        commands = []
        def respond(data):
            commands.append(data)
            threading.Timer(0.04, lambda: self.mock.feed(b"OK\n$ ")).start()
        self.mock.on_write = respond
        replies = []
        first = threading.Thread(target=lambda: replies.append(board.command("first")))
        second = threading.Thread(target=lambda: replies.append(board.command("second")))
        first.start()
        second.start()
        first.join(1)
        second.join(1)
        self.assertEqual(len(replies), 2)
        self.assertEqual(len(commands), 2)
        self.assertGreaterEqual(self.mock.writes[1][0] - self.mock.writes[0][0], 0.03)

    def test_expired_marker_is_explicit_and_offsets_stay_monotonic(self):
        board = self.board(max_buffer_bytes=1024)
        self.mock.feed(b"a" * 1500 + b"\n")
        with board._condition:
            self.assertTrue(board._condition.wait_for(lambda: board.marker() == 1501, timeout=1))
        self.assertEqual(board.marker(), 1501)
        with self.assertRaisesRegex(ValueError, "expired"):
            board.read_since(0)

    def test_close_unblocks_wait_and_is_idempotent(self):
        board = self.board()
        exceptions = []
        def wait():
            try:
                board.read_until("never", since=0, timeout=10)
            except ConnectionError as exc:
                exceptions.append(exc)
        waiter = threading.Thread(target=wait)
        waiter.start()
        board.close()
        board.close()
        waiter.join(1)
        self.assertFalse(waiter.is_alive())
        self.assertFalse(board._reader.is_alive())
        self.assertTrue(self.mock.closed)
        self.assertEqual(len(exceptions), 1)


if __name__ == "__main__":
    unittest.main()
