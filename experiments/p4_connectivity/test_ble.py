#!/usr/bin/env python3
"""Encrypted HardwareOne BLE tests using the S3 as a USB-connected radio.

The Mac never accesses Bluetooth. Import and --help never open serial ports.
run_probe() reuses an already open, authenticated MeshConsole owned by the caller.
The standalone --physical CLI opens only the S3 USB port and requires provisioned
firmware. Credentials remain in private JSON; no BLE secret crosses USB in
plaintext (only the separate S3 serial login uses the local admin password).
"""

from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import importlib
import importlib.metadata
import json
import logging
import os
from pathlib import Path
import secrets
import sys
import time

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
SERVICE = "12345678-1234-5678-1234-56789abcdef0"
STATUS_CHAR = "12345678-1234-5678-1234-56789abcde03"
INFO_CHARS = {"manufacturer": "00002a29-0000-1000-8000-00805f9b34fb",
              "model": "00002a24-0000-1000-8000-00805f9b34fb",
              "firmware": "00002a26-0000-1000-8000-00805f9b34fb"}
DEFAULT_MACS = {"p4": "fc:01:2c:e0:b9:a8", "s3": "68:ee:8f:50:e9:d0"}


class BleCheckFailed(Exception):
    """A safely reportable test failure."""


def require(condition, message):
    if not condition:
        raise BleCheckFailed(message)


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def load_companion():
    # Reuse the repository's audited protocol/client instead of reproducing
    # X25519/PSK/AEAD framing here. Dependencies are lazy for import-safe tests.
    directory = str(ROOT / "tools")
    if directory not in sys.path:
        sys.path.insert(0, directory)
    os.environ.pop("BLEAK_LOGGING", None)  # never enable raw GATT debug logging
    logging.getLogger("bleak").setLevel(logging.CRITICAL)
    return importlib.import_module("ble_secure_event_catalog_client")


def preflight():
    result = {"python": sys.version.split()[0], "transport": "s3-usb-gatt", "radioAccessed": False, "packages": {}}
    for name in ("PyNaCl", "pyserial"):
        try:
            result["packages"][name] = importlib.metadata.version(name)
        except importlib.metadata.PackageNotFoundError:
            result["packages"][name] = None
    return result


class PrivateReport:
    """Redact complete records before serialization; raw fragments stay in RAM."""

    def __init__(self, run_dir, credentials):
        self.directory = Path(run_dir)
        self.directory.mkdir(mode=0o700)
        self.secrets = sorted({value for value in credentials.values() if isinstance(value, str) and value}, key=len, reverse=True)
        fd = os.open(self.directory / "ble.log", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        self.log = os.fdopen(fd, "w", encoding="utf-8")
        self.results = {"schema": 1, "status": "running", "started": utc_now(), "checks": []}

    def safe(self, value):
        if isinstance(value, str):
            for secret in self.secrets:
                value = value.replace(secret, "[REDACTED]")
            return value
        if isinstance(value, dict):
            return {self.safe(str(key)): self.safe(item) for key, item in value.items()}
        if isinstance(value, (tuple, list)):
            return [self.safe(item) for item in value]
        return value

    def event(self, kind, **evidence):
        self.log.write(json.dumps(self.safe({"time": utc_now(), "kind": kind, **evidence})) + "\n")
        self.log.flush()

    def save(self):
        fd = os.open(self.directory / "results.json", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as output:
            json.dump(self.safe(self.results), output, indent=2)
            output.write("\n")

    def close(self):
        self.log.close()


def parse_object(payload, required):
    try:
        value = json.loads(payload)
    except (ValueError, TypeError):
        raise BleCheckFailed("Expected a complete JSON command reply") from None
    require(isinstance(value, dict) and required in value, "Command reply has an unexpected JSON schema")
    return value


def validate_system(value, firmware):
    require(value.get("schema") == 1 and value.get("fw") == firmware, "System/GATT firmware versions differ")
    require(isinstance(value.get("board"), str) and value["board"], "System reply lacks board identity")
    require(isinstance(value.get("uptime_hms"), str), "System reply lacks uptime")
    return value


def validate_ble(value, username):
    require(value.get("schema") == 1 and value.get("initialized") is True, "BLE status is not initialized")
    require(value.get("activeConnections", 0) >= 1, "BLE status lacks active connection")
    require(any(row.get("user") == username for row in value.get("connections", [])), "BLE status lacks the authenticated user")
    return value




class SecureConnection:
    def __init__(self, companion, client_type, backend, device, credentials, report, timeout):
        self.lib, self.client_type, self.backend = companion, client_type, backend
        self.device, self.credentials, self.report, self.timeout = device, credentials, report, timeout
        self.client = self.channel = self.pump = None
        self.notify_started = False
        self.commands = asyncio.Lock()
        self.reply_evidence = []

    async def __aenter__(self):
        try:
            self.channel = self.lib.SecureChannelV1Client(self.credentials["ble_secret"], reassembly_timeout=5)
            self.generation, hello = self.channel.begin_handshake()
            self.pump = self.lib._NotificationPump(asyncio.get_running_loop(), object(), self.generation)
            self.client = self.client_type(self.device, disconnected_callback=self.pump.disconnected, timeout=self.timeout)
            await asyncio.wait_for(self.client.connect(), self.timeout)
            require(self.client.is_connected, "BLE connection did not reach connected state")
            self.request, self.response = self.lib._find_command_characteristics(self.client)
            self.mtu = await self.lib._require_secure_mtu(self.client, self.backend)
            self.info = {}
            for key, uuid in INFO_CHARS.items():
                raw = await asyncio.wait_for(self.client.read_gatt_char(uuid), self.timeout)
                self.info[key] = bytes(raw).decode("utf-8", errors="strict")
                require(bool(self.info[key]), "An empty Device Information characteristic was returned")
            require(self.info["manufacturer"] == "HardwareOne", "Unexpected Device Information manufacturer")
            self.initial_status = await self.read_status()
            await asyncio.wait_for(self.client.start_notify(self.response, self.pump.callback), self.timeout)
            self.notify_started = True
            await asyncio.wait_for(self.lib._establish_channel(self.client, self.request, self.pump, self.channel,
                                                               self.generation, hello, self.timeout), self.timeout)
            self.report.event("secure_channel_established", identifier=str(self.device.address), mtu=self.mtu, deviceInfo=self.info)
            return self
        except BaseException:
            await self.__aexit__(*sys.exc_info())
            raise

    async def read_status(self):
        raw = await asyncio.wait_for(self.client.read_gatt_char(STATUS_CHAR), self.timeout)
        value = parse_object(bytes(raw), "state")
        require(value["state"] == "connected" and all(type(value.get(k)) is int for k in ("rx", "tx", "uptime")),
                "Invalid GATT connection status")
        return value

    async def command(self, text, *, expected=None, field=None, login=False):
        async with self.commands:
            label = "login [REDACTED]" if login else text
            self.report.event("command", command=label)
            command = self.lib.build_login_command(self.credentials["username"], self.credentials["password"]) if login else bytearray(text.encode())
            await asyncio.wait_for(self.lib._send_encrypted_command(self.client, self.request, self.channel,
                                                                     self.generation, command), self.timeout)
            deadline = time.monotonic() + self.timeout
            while True:
                reply = await self.lib._receive_complete_reply(self.pump, self.channel, self.generation, deadline)
                require(reply.is_text, "Unexpected binary reply to a text command")
                payload = reply.payload.decode("utf-8", errors="strict")
                self.report.event("encrypted_reply", payload=payload, messageId=reply.message_id,
                                  fragmentCount=reply.fragment_count, firstCounter=reply.first_counter, lastCounter=reply.last_counter)
                if self.lib.is_unsolicited_notification(reply.payload):
                    continue
                if login:
                    matched = self.lib.classify_login_reply(reply.payload, self.credentials["username"])
                elif expected is not None:
                    matched = payload.strip() == expected
                else:
                    try:
                        matched = field in json.loads(payload) if field else True
                    except (ValueError, TypeError):
                        matched = False
                if not matched:
                    require(not payload.startswith(("Error", "ERROR", "Unknown command", "Authentication required", "[ble] Authentication failed")),
                            "Firmware rejected BLE command")
                    continue
                self.reply_evidence.append({"command": label, "encrypted": True, "messageId": reply.message_id,
                                            "fragments": reply.fragment_count, "firstCounter": reply.first_counter,
                                            "lastCounter": reply.last_counter, "bytes": len(reply.payload)})
                return parse_object(payload, field) if field else payload.strip()

    async def __aexit__(self, exc_type, exc, traceback):
        cleanup_error = None
        if self.pump is not None:
            self.pump.deactivate()
        if self.client is not None and (self.client.is_connected or getattr(self.client, "connect_attempted", False)):
            if self.notify_started:
                try:
                    await asyncio.wait_for(self.client.stop_notify(self.response), 5)
                except Exception:
                    pass
            try:
                await asyncio.wait_for(self.client.disconnect(), 10)
                require(not self.client.is_connected, "Client remained connected after disconnect")
                self.report.event("disconnected", identifier=str(self.device.address))
            except Exception as error:
                cleanup_error = error
        if self.channel is not None:
            try:
                self.channel.reset()
            except Exception as error:
                cleanup_error = cleanup_error or error
        if exc_type is None and cleanup_error is not None:
            raise cleanup_error


async def exercise_connection(connection, expected_mac, username):
    # Both first connection and reconnection must start unauthenticated. This
    # proves named login does not accidentally survive a connId reincarnation.
    await connection.command("whoami", expected="You are (unknown)")
    await connection.command("status json", expected="Authentication required. Use: login <username> <password>")
    await connection.command("login", login=True)
    await connection.command("whoami", expected=f"You are {username} (admin)")
    system = validate_system(await connection.command("status json", field="fw"), connection.info["firmware"])
    ble = validate_ble(await connection.command("blestatus json", field="connections"), username)
    mesh = await connection.command("espnowstatus json", field="mac")
    require(mesh.get("schema") == 1 and mesh.get("initialized") is True, "ESP-NOW is not initialized during BLE test")
    require(str(mesh.get("mac", "")).lower() == expected_mac.lower(), "Wrong board: ESP-NOW MAC differs from selected target")
    status = await connection.read_status()
    require(status["rx"] > connection.initial_status["rx"] and status["tx"] > connection.initial_status["tx"],
            "GATT command/response counters did not advance")
    connection.channel.assert_complete(connection.generation)
    return {"deviceInfo": connection.info, "mtu": connection.mtu, "secureChannel": "v1",
            "initiallyUnauthenticated": True, "preLoginCommandDenied": True, "namedAdminLogin": True,
            "system": system, "bleStatus": ble, "meshStatus": mesh, "gattCounters": status,
            "encryptedReplies": connection.reply_evidence}


async def run_probe(board, credentials, expected_name="HW1_P4_BLE", *, expected_mac=DEFAULT_MACS["p4"], expected_ble_mac=None,
                    run_root=None, timeout=65, scan_seconds=5, on_connected=None):
    """Use caller-owned S3 MeshConsole; never close it or open another port.

    The console must already have the named serial admin login. Optional
    expected_ble_mac selects an explicitly observed BLE advertisement address
    when the advertised name is omitted. It is independent of expected_mac.
    Optional
    on_connected(connection, cycle) may be sync or async; use it to exercise
    mesh/HTTP while the BLE link is live. Its returned evidence is redacted.
    Returns a sanitized report dict with resultPath, or raises BleCheckFailed.
    """
    from serial_gatt import SerialGattClient, scan, rpc
    import inspect
    require(isinstance(credentials, dict), "Credentials must be a JSON object")
    for key in ("username", "password", "ble_secret"):
        require(isinstance(credentials.get(key), str) and bool(credentials[key]), f"Private credentials missing {key}")
    companion = load_companion()
    root = Path(run_root) if run_root else HERE / "private" / "ble-runs"
    root.mkdir(parents=True, exist_ok=True, mode=0o700)
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
    report = PrivateReport(root / f"{stamp}-{secrets.token_hex(4)}", credentials)
    report.results.update(transport="Board GATT central through caller-owned USB serial", targetName=expected_name, expectedMeshMac=expected_mac, preflight=preflight())
    report.save()
    try:
        identity = await asyncio.to_thread(board.command, "whoami", timeout=timeout)
        require(f"You are {credentials['username']} (admin)" in identity, "S3 serial console is not logged in as the expected admin")
        state = await rpc(board, "status", timeout)
        require(state.get("initialized") is True, "S3 BLE core must already be initialized with openble")
        require(not state["connected"], "S3 bridge already has a connection; no implicit disconnect performed")
        device = await scan(board, expected_name, scan_seconds, expected_ble_mac=expected_ble_mac)
        report.results["peripheral"] = {"name": device.name, "bleMac": device.address, "addressType": device.address_type,
                                         "selector": "observedBleMac" if expected_ble_mac is not None else "advertisedName"}
        client_type = lambda device, **kwargs: SerialGattClient(board, device, **kwargs)
        first_info = None
        for cycle in ("initial", "reconnect"):
            print(f"RUN p4_ble_{cycle}", flush=True)
            started = time.monotonic()
            try:
                async with SecureConnection(companion, client_type, None, device, credentials, report, timeout) as connection:
                    evidence = await exercise_connection(connection, expected_mac, credentials["username"])
                    if first_info is not None:
                        require(evidence["deviceInfo"] == first_info, "Device Information changed across reconnect")
                    first_info = evidence["deviceInfo"]
                    if on_connected is not None:
                        extra = on_connected(connection, cycle)
                        if inspect.isawaitable(extra):
                            extra = await extra
                        evidence["whileConnected"] = extra
                        # Verify BLE still works after the caller's mesh/HTTP work.
                        await connection.command("whoami", expected=f"You are {credentials['username']} (admin)")
                report.results["checks"].append({"name": cycle, "status": "passed", "durationSec": round(time.monotonic() - started, 3), "evidence": evidence})
                report.save()
                print(f"PASS p4_ble_{cycle}", flush=True)
            except BaseException as error:
                report.results["checks"].append({"name": cycle, "status": "failed", "error": str(error)})
                raise
            await asyncio.sleep(2)
        report.results.update(status="passed", finished=utc_now())
        report.save()
        return report.safe({**report.results, "resultPath": str(report.directory / "results.json")})
    except BaseException as error:
        report.results.update(status="failed", error=str(error), finished=utc_now())
        report.event("failure", error=str(error), errorType=type(error).__name__)
        report.save()
        raise BleCheckFailed(report.safe(str(error))) from None
    finally:
        report.close()


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--preflight", action="store_true", help="Read dependencies only; no serial or radio access")
    mode.add_argument("--physical", action="store_true", help="Open S3 USB serial and use its BLE radio")
    parser.add_argument("--credentials", type=Path, default=HERE / "private" / "credentials.json")
    parser.add_argument("--run-root", type=Path, default=HERE / "private" / "ble-runs")
    parser.add_argument("--s3-port", default="/dev/cu.usbmodem1101")
    parser.add_argument("--name", default="HW1_P4_BLE", help="Exact BLE advertised server name")
    parser.add_argument("--ble-mac", help="Explicit observed BLE MAC, taking precedence over advertised name")
    parser.add_argument("--expected-mac", default=DEFAULT_MACS["p4"], help="Server ESP-NOW MAC for board identity check")
    parser.add_argument("--scan-seconds", type=int, choices=range(1, 11), default=5)
    parser.add_argument("--timeout", type=float, default=65)
    return parser.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    if args.preflight:
        print(json.dumps(preflight(), indent=2))
        return 0
    private_values = []
    def safe(value):
        for secret in sorted(private_values, key=len, reverse=True):
            value = value.replace(secret, "[REDACTED]")
        return value
    try:
        credentials = json.loads(args.credentials.read_text())
        require(isinstance(credentials, dict), "Credentials must be a JSON object")
        private_values = [value for value in credentials.values() if isinstance(value, str) and value]
        sys.path.insert(0, str(HERE.parent / "p4_mesh"))
        from connectivity_redaction import ConnectivityConsole
        companion = load_companion()
        stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        log_root = HERE / "private" / "ble-serial-logs"
        log_root.mkdir(parents=True, exist_ok=True, mode=0o700)
        with ConnectivityConsole(args.s3_port, log_root / f"{stamp}-{secrets.token_hex(4)}.log", secrets=private_values, completion="hardwareone") as board:
            time.sleep(9)  # USB reopen resets these boards; let autostart finish.
            login = companion.build_login_command(credentials["username"], credentials["password"])
            try:
                board.command(login.decode(), timeout=args.timeout)
            finally:
                companion._wipe(login)
            result = asyncio.run(run_probe(board, credentials, args.name, expected_mac=args.expected_mac,
                                           expected_ble_mac=args.ble_mac, run_root=args.run_root,
                                           timeout=args.timeout, scan_seconds=args.scan_seconds))
        print(safe(f"PASS encrypted BLE through S3; results: {result['resultPath']}"))
        return 0
    except BaseException as error:
        print(safe(f"FAIL {type(error).__name__}: {error}"), file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1


if __name__ == "__main__":
    raise SystemExit(main())
