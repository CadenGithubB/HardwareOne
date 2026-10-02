"""Offline BLE test-harness tests. No CoreBluetooth managers or real ports."""
import asyncio
import base64
import json
from pathlib import Path
import stat
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import AsyncMock

from test_ble import (BleCheckFailed, PrivateReport, SecureConnection, SERVICE,
                      exercise_connection, parse_args, parse_object,
                      validate_ble, validate_system)
from serial_gatt import BridgeError, SerialGattClient, decode_bridge, scan


class BleParsers(unittest.TestCase):
    def test_schema_and_firmware_checks(self):
        self.assertEqual(parse_object(b'{"fw":"1"}', "fw"), {"fw": "1"})
        for value in (b'{"fw":', b'[]', b'{"other":1}'):
            with self.assertRaises(BleCheckFailed):
                parse_object(value, "fw")
        system = {"schema": 1, "fw": "7", "board": "P4", "uptime_hms": "0h 1m 2s"}
        self.assertEqual(validate_system(system, "7"), system)
        with self.assertRaises(BleCheckFailed):
            validate_system(system, "8")

    def test_named_auth_cannot_be_substituted_by_connection_count(self):
        value = {"schema": 1, "initialized": True, "activeConnections": 1, "connections": [{"user": "Unknown"}]}
        with self.assertRaises(BleCheckFailed):
            validate_ble(value, "tester")
        value["connections"][0]["user"] = "tester"
        self.assertEqual(validate_ble(value, "tester"), value)

    def test_redaction_before_json_encoding_and_private_modes(self):
        with tempfile.TemporaryDirectory() as parent:
            directory = Path(parent) / "run"
            secret = "private-ß-value"
            report = PrivateReport(directory, {"password": secret})
            report.event("reply", payload="prefix " + secret + " suffix")
            report.results[secret] = {"nested": [secret]}
            report.save()
            report.close()
            for filename in ("ble.log", "results.json"):
                path = directory / filename
                data = path.read_text()
                self.assertNotIn(secret, data)
                self.assertNotIn(json.dumps(secret)[1:-1], data)
                self.assertIn("[REDACTED]", data)
                self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)

    def test_bridge_errors_drops_and_incomplete_json_fail(self):
        good = {"schema": 1, "bridge": "s3-gatt-v1", "ok": True, "drops": 0}
        self.assertEqual(decode_bridge('$ ' + json.dumps(good) + '\n$ You are admin\n'), good)
        hosted = {**good, "bridge": "board-gatt-v1"}
        self.assertEqual(decode_bridge(json.dumps(hosted)), hosted)
        for value in ({**good, "drops": 1}, {**good, "ok": False, "error": "not_connected"}):
            with self.assertRaises(BridgeError):
                decode_bridge(json.dumps(value))
        with self.assertRaises(BridgeError):
            decode_bridge('{"schema":1,')

    def test_import_safe_explicit_physical_mode_and_identifiers(self):
        args = parse_args(["--physical", "--s3-port", "/dev/test", "--name", "P4"])
        self.assertEqual(args.s3_port, "/dev/test")
        self.assertEqual(args.name, "P4")
        self.assertFalse(args.preflight)


class BleAsyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_adapter_moves_complete_binary_frames_and_disconnects(self):
        class Console:
            def __init__(self):
                self.commands = []
                self.delivered = False
            def command(self, command, timeout):
                self.commands.append(command)
                value = {"schema": 1, "bridge": "s3-gatt-v1", "ok": True, "drops": 0,
                         "generation": 8, "connected": True, "queued": 0, "mtu": 247}
                if command.startswith("bleprobe poll"):
                    value["frames"] = [] if self.delivered else [{"generation": 8, "bytes": 3, "data": "AQID"}]
                    self.delivered = True
                elif command.startswith("bleprobe write"):
                    value["bytes"] = len(base64.b64decode(command.split()[-1]))
                elif command == "bleprobe disconnect":
                    value["connected"] = False
                return json.dumps(value)
        console = Console()
        device = SimpleNamespace(address="aa:bb", address_type=0)
        disconnected = []
        client = SerialGattClient(console, device, disconnected_callback=disconnected.append)
        await client.connect()
        response = client.services.get_characteristic("12345678-1234-5678-1234-56789abcde02")
        request = client.services.get_characteristic("12345678-1234-5678-1234-56789abcde01")
        received = asyncio.get_running_loop().create_future()
        await client.start_notify(response, lambda _, data: received.set_result(data))
        self.assertEqual(await asyncio.wait_for(received, 1), bytearray(b"\x01\x02\x03"))
        await client.write_gatt_char(request, b"\x10secret-is-encrypted-here", response=True)
        await client.disconnect()
        self.assertFalse(client.is_connected)
        self.assertIsNone(client.task)
        self.assertEqual(disconnected, [])
        self.assertTrue(any(command.startswith("bleprobe write ") for command in console.commands))
        self.assertEqual(console.commands[-1], "bleprobe disconnect")

    async def test_adapter_fences_changed_generation(self):
        client = SerialGattClient(None, None, disconnected_callback=lambda _: None)
        client.generation = 5
        with self.assertRaises(BridgeError):
            client.check_generation({"generation": 6, "connected": True})
        with self.assertRaises(BridgeError):
            client.check_generation({"generation": 5, "connected": False})

    async def test_scan_selects_exact_name_and_observed_address(self):
        class Console:
            def command(self, command, timeout):
                self.command_seen = command
                return '$ ' + json.dumps({"schema": 1, "bridge": "s3-gatt-v1", "ok": True, "drops": 0,
                    "devices": [{"mac": "aa:bb", "name": "Wanted", "addressType": 1},
                                {"mac": "cc:dd", "name": "Other", "addressType": 0}]})
        console = Console()
        device = await scan(console, "Wanted", 3)
        self.assertEqual(device.address, "aa:bb")
        self.assertEqual(device.address_type, 1)
        self.assertEqual(console.command_seen, "bleprobe scan 3")
        with self.assertRaises(BridgeError):
            await scan(console, "Want", 3)

    async def test_explicit_observed_ble_mac_selects_nameless_advertisement(self):
        class Console:
            def __init__(self):
                self.calls = 0
                self.rows = [{"mac": "02:48:57:31:01:aa", "name": "", "addressType": 0},
                             {"mac": "02:48:57:31:01:ab", "name": "HW1_P4_BLE", "addressType": 1}]
            def command(self, command, timeout):
                self.calls += 1
                return json.dumps({"schema": 1, "bridge": "s3-gatt-v1", "ok": True, "drops": 0, "devices": self.rows})
        console = Console()
        device = await scan(console, "HW1_P4_BLE", expected_ble_mac="02:48:57:31:01:AA")
        self.assertEqual(device.address, "02:48:57:31:01:aa")
        self.assertEqual(device.name, "")
        self.assertEqual(device.address_type, 0)
        with self.assertRaises(BridgeError):
            await scan(console, "HW1_P4_BLE", expected_ble_mac="02:48:57:31:01:a8")
        # Explicit selection must not fall back to the named but wrong device,
        # and malformed addresses must fail before touching the console.
        calls = console.calls
        with self.assertRaises(BridgeError):
            await scan(console, "HW1_P4_BLE", expected_ble_mac="0248573101aa")
        self.assertEqual(console.calls, calls)
        console.rows.append(dict(console.rows[0]))
        with self.assertRaises(BridgeError):
            await scan(console, "HW1_P4_BLE", expected_ble_mac="02:48:57:31:01:aa")

    async def test_command_waits_for_completed_notification_and_correct_reply(self):
        report = SimpleNamespace(event=lambda *a, **k: None)
        replies = iter([SimpleNamespace(is_text=True, payload=b"#NOTIF unrelated", message_id=1, fragment_count=1, first_counter=1, last_counter=1),
                        SimpleNamespace(is_text=True, payload=b"You are tester (admin)", message_id=2, fragment_count=2, first_counter=2, last_counter=3)])
        lib = SimpleNamespace(_send_encrypted_command=AsyncMock(),
                              _receive_complete_reply=AsyncMock(side_effect=lambda *a: next(replies)),
                              is_unsolicited_notification=lambda payload: payload.startswith(b"#NOTIF "))
        connection = SecureConnection(lib, None, None, None, {}, report, 1)
        connection.request = None
        connection.generation = 1
        result = await connection.command("whoami", expected="You are tester (admin)")
        self.assertEqual(result, "You are tester (admin)")
        self.assertEqual(lib._receive_complete_reply.await_count, 2)
        self.assertEqual(connection.reply_evidence[0]["fragments"], 2)
        self.assertTrue(connection.reply_evidence[0]["encrypted"])

    async def test_full_connection_workflow_checks_responses_and_board_identity(self):
        class Connection:
            info = {"firmware": "7"}
            mtu = 247
            initial_status = {"rx": 10, "tx": 10}
            generation = 1
            reply_evidence = []
            channel = SimpleNamespace(assert_complete=lambda generation: None)
            commands = []
            async def command(self, command, **kwargs):
                self.commands.append((command, kwargs))
                if kwargs.get("expected") is not None:
                    return kwargs["expected"]
                if command == "status json":
                    return {"schema": 1, "fw": "7", "board": "test", "uptime_hms": "0h 1m 2s"}
                if command == "blestatus json":
                    return {"schema": 1, "initialized": True, "activeConnections": 1, "connections": [{"user": "tester"}]}
                if command == "espnowstatus json":
                    return {"schema": 1, "initialized": True, "mac": "aa:bb"}
            async def read_status(self):
                return {"rx": 17, "tx": 26}
        connection = Connection()
        result = await exercise_connection(connection, "aa:bb", "tester")
        self.assertTrue(result["preLoginCommandDenied"])
        self.assertEqual([name for name, _ in connection.commands], ["whoami", "status json", "login", "whoami", "status json", "blestatus json", "espnowstatus json"])
        with self.assertRaises(BleCheckFailed):
            await exercise_connection(Connection(), "wrong", "tester")


if __name__ == "__main__":
    unittest.main()
