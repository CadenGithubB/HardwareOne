"""Bleak-shaped adapter for the S3 USB-admin GATT bridge; no Mac Bluetooth APIs."""
from __future__ import annotations
import asyncio
import base64
from dataclasses import dataclass
import json
import re

SERVICE = "12345678-1234-5678-1234-56789abcdef0"
REQUEST = "12345678-1234-5678-1234-56789abcde01"
RESPONSE = "12345678-1234-5678-1234-56789abcde02"
STATUS = "12345678-1234-5678-1234-56789abcde03"
READS = {"00002a29-0000-1000-8000-00805f9b34fb": "manufacturer",
         "00002a24-0000-1000-8000-00805f9b34fb": "model",
         "00002a26-0000-1000-8000-00805f9b34fb": "firmware", STATUS: "status"}


class BridgeError(Exception):
    pass


@dataclass
class Peripheral:
    address: str
    name: str
    address_type: int


@dataclass
class Characteristic:
    uuid: str
    service_uuid: str
    properties: list


class Services:
    """Descriptors confirmed by firmware during each connect, not fake devices."""
    def get_service(self, uuid):
        return self if uuid == SERVICE else None
    def get_characteristic(self, uuid):
        if uuid == REQUEST:
            return Characteristic(uuid, SERVICE, ["write"])
        if uuid == RESPONSE:
            return Characteristic(uuid, SERVICE, ["notify"])
        return None


def decode_bridge(output):
    # CLI prompts and audit messages can surround the one bridge JSON response.
    decoder = json.JSONDecoder()
    for position, char in enumerate(output):
        if char != "{":
            continue
        try:
            value, _ = decoder.raw_decode(output[position:])
        except ValueError:
            continue
        if isinstance(value, dict) and value.get("schema") == 1 and (value.get("bridge") in ("s3-gatt-v1", "board-gatt-v1") or "error" in value):
            if value.get("ok") is not True:
                raise BridgeError("S3 bridge rejected operation: " + str(value.get("error", "unknown")))
            if value.get("drops") != 0:
                raise BridgeError("S3 bridge dropped notification records")
            return value
    raise BridgeError("S3 bridge did not return a complete JSON response")


async def rpc(board, command, timeout=65):
    # Cancelling asyncio.to_thread does not cancel the actual serial operation.
    # Keep ownership until its command/barrier has finished, including repeated
    # cancellation while cleanup is waiting. Otherwise a late connect or poll
    # can mutate firmware/read its queue after the test has already returned.
    worker = asyncio.create_task(asyncio.to_thread(board.command, "bleprobe " + command, timeout=timeout))
    cancelled = False
    while not worker.done():
        try:
            await asyncio.shield(worker)
        except asyncio.CancelledError:
            cancelled = True
        except Exception:
            break
    if cancelled:
        # Retrieve exceptions to prevent an unobserved-task warning; preserve
        # cancellation only after ownership of the serial command is settled.
        try:
            worker.result()
        except BaseException:
            pass
        raise asyncio.CancelledError
    output = worker.result()
    return decode_bridge(output)


async def scan(board, expected_name, seconds=5, *, expected_ble_mac=None):
    if expected_ble_mac is not None and (not isinstance(expected_ble_mac, str) or
            re.fullmatch(r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", expected_ble_mac) is None):
        raise BridgeError("Explicit BLE selector must be an observed colon-separated MAC address")
    result = await rpc(board, f"scan {seconds}")
    # Some advertisements carry the command UUID but omit a name when two
    # 128-bit services exhaust the legacy advertisement budget. An explicitly
    # observed BLE address takes precedence; never derive it from the STA MAC.
    matches = [row for row in result["devices"] if
               (str(row.get("mac", "")).lower() == expected_ble_mac.lower() if expected_ble_mac is not None
                else row.get("name") == expected_name)]
    if len(matches) != 1:
        raise BridgeError("Expected exactly one S3-discovered BLE peripheral matching the explicit BLE address" if expected_ble_mac is not None
                          else "Expected exactly one S3-discovered BLE peripheral with the requested name")
    row = matches[0]
    return Peripheral(row["mac"], row["name"], row["addressType"])


class SerialGattClient:
    def __init__(self, board, device, *, disconnected_callback, timeout=65):
        self.board, self.device, self.timeout = board, device, timeout
        self.disconnected_callback = disconnected_callback
        self.is_connected = False
        self.mtu_size = 0
        self.services = Services()
        self.task = None
        self.poll_error = None
        self.generation = None
        self.expected_disconnect = False
        self.connect_attempted = False

    async def connect(self):
        self.connect_attempted = True
        result = await rpc(self.board, f"connect {self.device.address} {self.device.address_type}", self.timeout)
        self.generation = result["generation"]
        self.mtu_size = result["mtu"]
        self.is_connected = result["connected"]

    def healthy(self):
        if self.poll_error:
            raise self.poll_error
        if not self.is_connected:
            raise BridgeError("S3 BLE link is not connected")

    def check_generation(self, result):
        if result["generation"] != self.generation or not result["connected"]:
            raise BridgeError("S3 BLE connection changed during secure session")

    async def read_gatt_char(self, characteristic):
        self.healthy()
        if characteristic not in READS:
            raise BridgeError("Read outside the fixed HardwareOne characteristic allowlist")
        result = await rpc(self.board, "read " + READS[characteristic], self.timeout)
        self.check_generation(result)
        return bytearray(base64.b64decode(result["data"], validate=True))

    async def write_gatt_char(self, characteristic, record, *, response):
        self.healthy()
        if characteristic.uuid != REQUEST or response is not True:
            raise BridgeError("Only acknowledged command-record writes are supported")
        result = await rpc(self.board, "write " + base64.b64encode(record).decode("ascii"), self.timeout)
        self.check_generation(result)
        if result["bytes"] != len(record):
            raise BridgeError("S3 bridge wrote a partial command record")

    async def start_notify(self, characteristic, callback):
        self.healthy()
        if characteristic.uuid != RESPONSE or self.task:
            raise BridgeError("Invalid/duplicate response subscription")
        result = await rpc(self.board, "subscribe", self.timeout)
        self.check_generation(result)
        self.task = asyncio.create_task(self.poll(characteristic, callback))

    async def poll(self, characteristic, callback):
        try:
            while True:
                result = await rpc(self.board, "poll 4", self.timeout)
                self.check_generation(result)
                for frame in result["frames"]:
                    if frame["generation"] != self.generation:
                        raise BridgeError("Stale S3 notification generation")
                    data = base64.b64decode(frame["data"], validate=True)
                    if len(data) != frame["bytes"]:
                        raise BridgeError("S3 notification length mismatch")
                    callback(characteristic, bytearray(data))
                await asyncio.sleep(0.025 if result["queued"] else 0.1)
        except asyncio.CancelledError:
            raise
        except Exception as error:
            self.poll_error = error
            self.disconnected_callback(self)

    async def stop_notify(self, characteristic):
        if self.task:
            task, self.task = self.task, None
            task.cancel()
            await asyncio.gather(task, return_exceptions=True)
        # Firmware closes the CCCD with the link; no shared-core teardown.

    async def disconnect(self):
        await self.stop_notify(None)
        result = await rpc(self.board, "disconnect", self.timeout)
        self.is_connected = bool(result["connected"])
        if self.is_connected:
            raise BridgeError("S3 bridge remained connected after disconnect")
        self.connect_attempted = False
