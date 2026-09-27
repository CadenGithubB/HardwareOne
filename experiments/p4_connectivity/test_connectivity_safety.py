"""Offline regressions for coordinator redaction, literal secrets and RPC close."""
import asyncio
import base64
import json
from pathlib import Path
import re
import tempfile
import time
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from board_control import raw_ble_secret
from connectivity_redaction import (ConnectivityConsole, ConnectivityConsoleFatal,
                                     clean_private_log, redact_http_bodies)
from console import ConsoleTimeout, MeshConsole
from serial_gatt import SerialGattClient, Peripheral, rpc
from test_ble import SecureConnection


class LoggingTests(unittest.TestCase):
    def new_console_without_port(self):
        console = ConnectivityConsole.__new__(ConnectivityConsole)
        console._transaction_lock = threading.RLock()
        console._fatal_console = False
        console._recovery_timeout = 0.01
        console._completion = "hardwareone"
        return console

    def test_actual_timeout_drains_old_barrier_before_cleanup_is_allowed(self):
        console = self.new_console_without_port()
        original = ConsoleTimeout(20, 0.01, "partial")
        with patch.object(MeshConsole, "command", side_effect=[original, "closed"]), \
             patch.object(console, "read_until", return_value="$ You are admin\n") as drain:
            with self.assertRaises(ConsoleTimeout):
                console.command("bleprobe connect 00:11:22:33:44:55")
            self.assertFalse(console.fatal_console)
            self.assertEqual(drain.call_args.kwargs["since"], 20)
            self.assertEqual(console.command("bleprobe disconnect"), "closed")

    def test_unsettled_timeout_latches_fatal_and_never_sends_next_command(self):
        console = self.new_console_without_port()
        timeout = ConsoleTimeout(20, 0.01, "partial")
        with patch.object(MeshConsole, "command", side_effect=timeout) as send, \
             patch.object(console, "read_until", side_effect=timeout):
            with self.assertRaises(ConnectivityConsoleFatal):
                console.command("bleprobe connect 00:11:22:33:44:55")
            with self.assertRaises(ConnectivityConsoleFatal):
                console.command("bleprobe disconnect")
            self.assertTrue(console.fatal_console)
            self.assertEqual(send.call_count, 1)

    def test_ble_secret_is_literal_without_added_quotes(self):
        secret = "Generated-Ble_Test7!"
        self.assertEqual("blesecret " + raw_ble_secret(secret), "blesecret Generated-Ble_Test7!")
        for bad in ('"' + secret + '"', secret + "\n", " " + secret, secret + ";whoami", secret + "&&x", secret + "\\"):
            with self.assertRaises(ValueError):
                raw_ble_secret(bad)

    def test_encoded_http_body_is_omitted_but_raw_response_stays_available(self):
        secret = "private-user-not-at-base64-boundary"
        raw_body = ("You are " + secret + " (admin)").encode()
        raw = json.dumps({"schema": 1, "bridge": "s3-http-v1", "body": base64.b64encode(raw_body).decode(), "bytes": len(raw_body)})
        console = ConnectivityConsole.__new__(ConnectivityConsole)
        console._secret_pattern = re.compile(re.escape(secret))
        filtered = console.redact(raw)
        self.assertEqual(json.loads(filtered)["body"], "[HTTP body omitted]")
        self.assertEqual(base64.b64decode(json.loads(raw)["body"]), raw_body)
        self.assertEqual(json.loads(filtered)["bytes"], len(raw_body))
        escaped = json.dumps({"response": raw})
        nested = json.loads(redact_http_bodies(escaped))["response"]
        self.assertEqual(json.loads(nested)["body"], "[HTTP body omitted]")
        self.assertEqual(redact_http_bodies(filtered), filtered)

    def test_http_filter_leaves_ble_ciphertext_and_retroactively_cleans_private_log(self):
        ble = '{"bridge":"board-gatt-v1","data":"AQID"}'
        self.assertEqual(redact_http_bodies(ble), ble)
        with tempfile.TemporaryDirectory() as directory:
            private = Path(directory) / "private"
            private.mkdir()
            path = private / "old.log"
            path.write_text('RX {"bridge":"s3-http-v1","body":"c2VjcmV0"}\n')
            self.assertEqual(clean_private_log(path), 1)
            self.assertNotIn("c2VjcmV0", path.read_text())
            self.assertEqual(clean_private_log(path), 0)


class CancellationTests(unittest.IsolatedAsyncioTestCase):
    async def test_cancelled_connect_settles_worker_then_closes_unconfirmed_link(self):
        class Board:
            def __init__(self):
                self.commands = []
                self.completed = False
                self.connected = False
            def command(self, command, timeout):
                self.commands.append(command)
                if command.startswith("bleprobe connect"):
                    time.sleep(0.03)
                    self.completed = self.connected = True
                elif command == "bleprobe disconnect":
                    self.connected = False
                return json.dumps({"schema": 1, "bridge": "board-gatt-v1", "ok": True, "drops": 0,
                                   "generation": 4, "mtu": 517, "connected": self.connected})
        board = Board()
        client = SerialGattClient(board, Peripheral("00:11:22:33:44:55", "test", 0), disconnected_callback=lambda _: None)
        with self.assertRaises(asyncio.TimeoutError):
            await asyncio.wait_for(client.connect(), 0.001)
        self.assertTrue(board.completed)
        self.assertTrue(board.connected)
        self.assertFalse(client.is_connected)  # cancellation prevented assignment
        report = SimpleNamespace(event=lambda *a, **kw: None)
        connection = SecureConnection(None, None, None, None, {}, report, 1)
        connection.client = client
        await connection.__aexit__(asyncio.TimeoutError, asyncio.TimeoutError(), None)
        self.assertFalse(board.connected)
        self.assertFalse(client.connect_attempted)
        self.assertEqual(board.commands[-1], "bleprobe disconnect")

    async def test_repeated_cancellation_cannot_abandon_serial_worker(self):
        class Board:
            done = False
            def command(self, command, timeout):
                time.sleep(0.03)
                self.done = True
                return json.dumps({"schema": 1, "bridge": "board-gatt-v1", "ok": True, "drops": 0})
        board = Board()
        task = asyncio.create_task(rpc(board, "poll 4"))
        await asyncio.sleep(0.003)
        task.cancel()
        await asyncio.sleep(0.003)
        task.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await task
        self.assertTrue(board.done)


if __name__ == "__main__":
    unittest.main()
