#!/usr/bin/env python3
"""Verify the System Event catalog through HardwareOne Secure BLE.

The default mode is entirely offline.  ``--physical`` is an explicit,
interactive opt-in: the client scans for an already-advertising HardwareOne
phone-server service, establishes Secure Channel v1, performs named login, and
requests ``events kinds json``.  It never changes the device's BLE role.

Secrets are accepted only from no-echo prompts and are never printed or stored.
Python strings cannot be reliably zeroed, but mutable UTF-8 command/secret
buffers are wiped as soon as their derived/encrypted value is available.
"""

from __future__ import annotations

import argparse
import asyncio
import getpass
import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Optional, Sequence


TOOLS_DIR = Path(__file__).resolve().parent
BLE_SECURE_DIR = TOOLS_DIR / "ble_secure"
if str(BLE_SECURE_DIR) not in sys.path:
    sys.path.insert(0, str(BLE_SECURE_DIR))

from secure_channel_v1 import (  # noqa: E402
    ClientState,
    CompletedReply,
    IncompleteMessageError,
    REPLY_VERSION_TEXT,
    SC_DATA,
    SecureChannelError,
    SecureChannelV1Client,
    StaleSessionError,
)


COMMAND_SERVICE_UUID = "12345678-1234-5678-1234-56789abcdef0"
COMMAND_REQUEST_UUID = "12345678-1234-5678-1234-56789abcde01"
COMMAND_RESPONSE_UUID = "12345678-1234-5678-1234-56789abcde02"
DEFAULT_DEVICE_NAME = "HardwareOne"
CATALOG_COMMAND = b"events kinds json"
MIN_SECURE_CHANNEL_MTU = 230
EXPECTED_CATALOG_BYTES = 2_877
EXPECTED_FAMILY_COUNT = 12
EXPECTED_KIND_COUNT = 152
MAX_LOGIN_TOKEN_BYTES = 64
NOTIFICATION_QUEUE_CAPACITY = 512
REASSEMBLY_POLL_SECONDS = 0.25

FIXTURE_PATH = (
    TOOLS_DIR.parent
    / "components"
    / "hardwareone"
    / "test"
    / "host"
    / "fixtures"
    / "event_catalog_v1.json"
)

_KIND_RE = re.compile(r"^[a-z0-9_]+$")
_RESERVED_KIND_NAMES = frozenset({"boot", "none", "set", "patch", "all", "list"})


class CompanionError(Exception):
    """Expected, safe-to-display companion-client failure."""


class CatalogValidationError(CompanionError):
    """The catalog was incomplete, malformed, reordered, or otherwise wrong."""


class TransportError(CompanionError):
    """The BLE transport could not preserve the required session contract."""


class TransportDisconnected(TransportError):
    """The physical connection ended before the operation completed."""


@dataclass(frozen=True)
class CatalogSummary:
    family_count: int
    kind_count: int
    byte_count: int


def _wipe(value: Optional[bytearray]) -> None:
    if value is not None:
        for index in range(len(value)):
            value[index] = 0


def _reject_duplicate_keys(pairs: Sequence[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise CatalogValidationError(f"duplicate JSON object key {key!r}")
        result[key] = value
    return result


def _strict_json_loads(payload: bytes) -> Any:
    try:
        text = payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise CatalogValidationError("catalog reply is not valid UTF-8") from exc
    try:
        return json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except CatalogValidationError:
        raise
    except (TypeError, ValueError, json.JSONDecodeError) as exc:
        raise CatalogValidationError("catalog reply is not valid JSON") from exc


def _validate_catalog_object(value: Any, byte_count: int) -> CatalogSummary:
    if not isinstance(value, dict) or set(value) != {"families"}:
        raise CatalogValidationError("catalog root must contain only 'families'")
    families = value["families"]
    if not isinstance(families, list):
        raise CatalogValidationError("catalog 'families' must be an array")

    family_names: set[str] = set()
    kind_names: set[str] = set()
    kind_count = 0
    for family in families:
        if not isinstance(family, dict) or set(family) != {"n", "k"}:
            raise CatalogValidationError("each family must contain only 'n' and 'k'")
        name = family["n"]
        kinds = family["k"]
        if not isinstance(name, str) or not name:
            raise CatalogValidationError("every family name must be a nonempty string")
        folded_family = name.casefold()
        if folded_family in family_names:
            raise CatalogValidationError("catalog family names must be unique")
        family_names.add(folded_family)
        if not isinstance(kinds, list) or not kinds:
            raise CatalogValidationError("every family must have a nonempty kind array")
        for kind in kinds:
            if not isinstance(kind, str) or _KIND_RE.fullmatch(kind) is None:
                raise CatalogValidationError("event kinds must be lowercase snake_case")
            folded_kind = kind.casefold()
            if folded_kind in kind_names:
                raise CatalogValidationError("event kinds must be case-fold unique")
            if kind in _RESERVED_KIND_NAMES:
                raise CatalogValidationError(
                    "reserved control words and aliases are not wire kinds"
                )
            kind_names.add(folded_kind)
            kind_count += 1

    return CatalogSummary(len(families), kind_count, byte_count)


def _load_expected_catalog(
    fixture_path: Path = FIXTURE_PATH,
) -> tuple[Any, bytes, CatalogSummary]:
    try:
        pretty = fixture_path.read_bytes()
    except OSError as exc:
        raise CatalogValidationError("event catalog fixture is unavailable") from exc
    expected = _strict_json_loads(pretty)
    compact = json.dumps(
        expected, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    summary = _validate_catalog_object(expected, len(compact))
    if summary != CatalogSummary(
        EXPECTED_FAMILY_COUNT, EXPECTED_KIND_COUNT, EXPECTED_CATALOG_BYTES
    ):
        raise CatalogValidationError("checked-in event catalog fixture has drifted")
    return expected, compact, summary


def validate_live_catalog(
    payload: bytes, fixture_path: Path = FIXTURE_PATH
) -> CatalogSummary:
    """Require valid v1 JSON and exact parity with the frozen compact fixture."""

    actual = _strict_json_loads(payload)
    actual_summary = _validate_catalog_object(actual, len(payload))
    expected, compact, expected_summary = _load_expected_catalog(fixture_path)
    if actual != expected:
        raise CatalogValidationError("live catalog names or family order differ from fixture")
    if payload != compact:
        raise CatalogValidationError("live catalog bytes differ from canonical compact v1 JSON")
    if actual_summary != expected_summary:
        raise CatalogValidationError("live catalog counts or byte length differ from fixture")
    return actual_summary


def _decode_text_reply(payload: bytes) -> str:
    try:
        return payload.decode("utf-8", errors="strict")
    except UnicodeDecodeError as exc:
        raise TransportError("device marked a non-UTF-8 message as text") from exc


def is_unsolicited_notification(payload: bytes) -> bool:
    """Recognize notification records before auth/catalog response parsing."""

    return payload.startswith(b"#NOTIF ")


def classify_login_reply(payload: bytes, username: str) -> Optional[bool]:
    """Return True for the named login reply; ignore unrelated complete text.

    Unsolicited notifications are consumed first so they still advance the
    authenticated D2C counter/msgId stream but can never become a login result.
    """

    if is_unsolicited_notification(payload):
        return None
    text = _decode_text_reply(payload)
    expected = f"[ble] Login successful. User: {username}"
    if text == expected or text == expected + " (admin)":
        return True
    if text == "[ble] Authentication failed.":
        raise CompanionError("named BLE login was rejected")
    if text in {
        "Usage: login <username> <password>",
        "Authentication required. Use: login <username> <password>",
    }:
        raise CompanionError("device rejected the named BLE login command")
    return None


def classify_catalog_reply(
    payload: bytes, fixture_path: Path = FIXTURE_PATH
) -> Optional[CatalogSummary]:
    """Accept only the catalog response; consume unrelated complete messages."""

    if is_unsolicited_notification(payload):
        return None
    if payload.startswith((b"Error", b"ERROR", b"Unknown command")):
        raise CompanionError("device rejected 'events kinds json'")
    if not payload.lstrip().startswith(b"{"):
        return None
    try:
        candidate = _strict_json_loads(payload)
    except CatalogValidationError:
        # Other subsystems may emit complete JSON-shaped messages.  Only a
        # response that claims the catalog root is owned by this request.
        if b'"families"' in payload[:256]:
            raise
        return None
    if not isinstance(candidate, dict) or "families" not in candidate:
        return None
    return validate_live_catalog(payload, fixture_path)


def _login_token(value: str, label: str) -> bytes:
    if not isinstance(value, str) or not value:
        raise CompanionError(f"{label} must not be empty")
    try:
        encoded = value.encode("ascii", errors="strict")
    except UnicodeEncodeError as exc:
        raise CompanionError(f"{label} must use printable ASCII for BLE login") from exc
    if len(encoded) > MAX_LOGIN_TOKEN_BYTES:
        raise CompanionError(f"{label} exceeds the firmware's 64-byte limit")
    if any(byte < 0x20 or byte > 0x7E for byte in encoded) or b'"' in encoded:
        raise CompanionError(
            f"{label} contains bytes the firmware's quoted login grammar cannot represent"
        )
    return encoded


def build_login_command(username: str, password: str) -> bytearray:
    user = _login_token(username, "username")
    secret = _login_token(password, "password")
    command = bytearray(b'login "')
    command.extend(user)
    command.extend(b'" "')
    command.extend(secret)
    command.extend(b'"')
    return command


class _NotificationPump:
    """Bounded notification handoff fenced by connection and SC generation."""

    def __init__(
        self,
        loop: asyncio.AbstractEventLoop,
        connection_token: object,
        secure_generation: int,
        capacity: int = NOTIFICATION_QUEUE_CAPACITY,
    ) -> None:
        self._loop = loop
        self._connection_token: Optional[object] = connection_token
        self._secure_generation = secure_generation
        self._queue: asyncio.Queue[tuple[object, int, bytes]] = asyncio.Queue(capacity)
        self._failure: asyncio.Future[BaseException] = loop.create_future()

    def callback(self, _characteristic: Any, data: bytearray) -> None:
        frame = bytes(data)
        token = self._connection_token
        generation = self._secure_generation
        self._loop.call_soon_threadsafe(
            self._enqueue, token, generation, frame
        )

    def disconnected(self, _client: Any) -> None:
        self._loop.call_soon_threadsafe(
            self.fail,
            TransportDisconnected("BLE connection closed before verification completed"),
        )

    def _enqueue(
        self, token: Optional[object], generation: int, frame: bytes
    ) -> None:
        if self._failure.done():
            return
        if token is None or token is not self._connection_token:
            self.fail(StaleSessionError("late callback from a closed BLE connection"))
            return
        if generation != self._secure_generation:
            self.fail(StaleSessionError("late callback from a replaced Secure session"))
            return
        try:
            self._queue.put_nowait((token, generation, frame))
        except asyncio.QueueFull:
            self.fail(TransportError("BLE notification queue overflowed"))

    def fail(self, error: BaseException) -> None:
        if not self._failure.done():
            self._failure.set_result(error)

    def deactivate(self) -> None:
        self._connection_token = None

    async def get(self, timeout: float) -> tuple[bytes, int]:
        queue_task = asyncio.create_task(self._queue.get())
        try:
            done, _ = await asyncio.wait(
                (queue_task, self._failure),
                timeout=timeout,
                return_when=asyncio.FIRST_COMPLETED,
            )
            if self._failure in done:
                queue_task.cancel()
                await asyncio.gather(queue_task, return_exceptions=True)
                raise self._failure.result()
            if queue_task not in done:
                queue_task.cancel()
                await asyncio.gather(queue_task, return_exceptions=True)
                raise asyncio.TimeoutError
            token, generation, frame = queue_task.result()
            if token is not self._connection_token:
                raise StaleSessionError("queued frame belongs to a closed BLE connection")
            if generation != self._secure_generation:
                raise StaleSessionError("queued frame belongs to a replaced Secure session")
            return frame, generation
        finally:
            if not queue_task.done():
                queue_task.cancel()


def _load_bleak() -> tuple[Any, Any, Any]:
    try:
        from bleak import BleakBackend, BleakClient, BleakScanner
    except ImportError as exc:
        raise CompanionError(
            "physical mode requires the hash-pinned tools/ble_secure environment"
        ) from exc
    return BleakBackend, BleakClient, BleakScanner


async def _scan_phone_server(
    scanner_type: Any,
    *,
    device_name: str,
    address: Optional[str],
    timeout: float,
) -> Any:
    matches: dict[str, Any] = {}
    target_seen_without_service = False
    wanted_address = address.casefold() if address else None

    def detected(device: Any, advertisement: Any) -> None:
        nonlocal target_seen_without_service
        identifier = str(device.address)
        local_name = advertisement.local_name or device.name or ""
        identity_matches = (
            identifier.casefold() == wanted_address
            if wanted_address is not None
            else local_name == device_name
        )
        if not identity_matches:
            return
        advertised_services = {
            str(value).casefold() for value in (advertisement.service_uuids or ())
        }
        if COMMAND_SERVICE_UUID in advertised_services:
            matches[identifier.casefold()] = device
        else:
            target_seen_without_service = True

    try:
        async with scanner_type(detected):
            await asyncio.sleep(timeout)
    except Exception as exc:
        raise TransportError("BLE scan failed") from exc

    if not matches:
        detail = (
            "matching device was seen without the command service"
            if target_seen_without_service
            else "no matching phone-server advertisement was found"
        )
        raise TransportError(
            f"{detail}; verify server role/advertising with authenticated UART "
            "and run openble there if needed"
        )
    if len(matches) != 1:
        raise TransportError(
            "multiple matching phone-server advertisements found; rerun with --address"
        )
    return next(iter(matches.values()))


def _find_command_characteristics(client: Any) -> tuple[Any, Any]:
    services = client.services
    service = services.get_service(COMMAND_SERVICE_UUID)
    request = services.get_characteristic(COMMAND_REQUEST_UUID)
    response = services.get_characteristic(COMMAND_RESPONSE_UUID)
    if service is None or request is None or response is None:
        raise TransportError("connected peripheral lacks the HardwareOne command service")
    request_service = str(getattr(request, "service_uuid", "")).casefold()
    response_service = str(getattr(response, "service_uuid", "")).casefold()
    if request_service != COMMAND_SERVICE_UUID or response_service != COMMAND_SERVICE_UUID:
        raise TransportError("command characteristics are attached to the wrong service")
    request_properties = {str(value).casefold() for value in request.properties}
    response_properties = {str(value).casefold() for value in response.properties}
    if not ({"write", "write-without-response"} & request_properties):
        raise TransportError("command request characteristic is not writable")
    if "notify" not in response_properties:
        raise TransportError("command response characteristic does not support notify")
    return request, response


async def _require_secure_mtu(client: Any, bleak_backend: Any) -> int:
    bluez = getattr(bleak_backend, "BLUEZ_DBUS", None)
    if bluez is not None and client.backend_id == bluez:
        acquire = getattr(getattr(client, "_backend", None), "_acquire_mtu", None)
        if acquire is None:
            raise TransportError("BlueZ backend cannot acquire the negotiated ATT MTU")
        try:
            await acquire()
        except Exception as exc:
            raise TransportError("BlueZ failed to acquire the negotiated ATT MTU") from exc
    try:
        mtu = int(client.mtu_size)
    except Exception as exc:
        raise TransportError("could not determine the negotiated ATT MTU") from exc
    if mtu < MIN_SECURE_CHANNEL_MTU:
        raise TransportError(
            f"negotiated ATT MTU {mtu} is below Secure Channel minimum "
            f"{MIN_SECURE_CHANNEL_MTU}"
        )
    return mtu


async def _next_frame(
    pump: _NotificationPump,
    channel: SecureChannelV1Client,
    generation: int,
    deadline: float,
    *,
    established: bool,
) -> bytes:
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            if established:
                channel.expire_reassembly(generation)
            raise TransportError("timed out waiting for a complete Secure BLE reply")
        wait_for = min(remaining, REASSEMBLY_POLL_SECONDS if established else remaining)
        try:
            frame, callback_generation = await pump.get(wait_for)
        except asyncio.TimeoutError:
            if established:
                channel.expire_reassembly(generation)
            continue
        except TransportDisconnected:
            if established:
                # Preserve the stronger incomplete-message diagnosis when the
                # disconnect interrupted an already-started application reply.
                channel.assert_complete(generation)
            raise
        if callback_generation != generation:
            raise StaleSessionError("notification crossed a Secure session generation")
        return frame


async def _receive_complete_reply(
    pump: _NotificationPump,
    channel: SecureChannelV1Client,
    generation: int,
    deadline: float,
) -> CompletedReply:
    while True:
        frame = await _next_frame(
            pump, channel, generation, deadline, established=True
        )
        if not frame or frame[0] != SC_DATA:
            raise TransportError("unexpected control frame after Secure establishment")
        reply = channel.receive_data(frame, generation)
        if reply is not None:
            return reply


async def _write_record(client: Any, characteristic: Any, record: bytes) -> None:
    try:
        await client.write_gatt_char(characteristic, record, response=True)
    except Exception as exc:
        raise TransportError("BLE command-record write failed") from exc


async def _establish_channel(
    client: Any,
    request: Any,
    pump: _NotificationPump,
    channel: SecureChannelV1Client,
    generation: int,
    hello: bytes,
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    await _write_record(client, request, hello)
    hello_ack = await _next_frame(
        pump, channel, generation, deadline, established=False
    )
    confirm = channel.receive_hello_ack(hello_ack, generation)
    await _write_record(client, request, confirm)
    confirm_ack = await _next_frame(
        pump, channel, generation, deadline, established=False
    )
    channel.receive_confirm_ack(confirm_ack, generation)


async def _send_encrypted_command(
    client: Any,
    request: Any,
    channel: SecureChannelV1Client,
    generation: int,
    command: bytearray,
) -> None:
    try:
        frame = channel.encrypt_command(command, generation)
    finally:
        _wipe(command)
    await _write_record(client, request, frame)


async def _wait_for_login(
    pump: _NotificationPump,
    channel: SecureChannelV1Client,
    generation: int,
    username: str,
    timeout: float,
) -> None:
    deadline = time.monotonic() + timeout
    while True:
        reply = await _receive_complete_reply(pump, channel, generation, deadline)
        if reply.version != REPLY_VERSION_TEXT:
            raise TransportError("unexpected binary message while waiting for login")
        if classify_login_reply(reply.payload, username):
            return


async def _wait_for_catalog(
    pump: _NotificationPump,
    channel: SecureChannelV1Client,
    generation: int,
    timeout: float,
) -> CatalogSummary:
    deadline = time.monotonic() + timeout
    while True:
        reply = await _receive_complete_reply(pump, channel, generation, deadline)
        if reply.version != REPLY_VERSION_TEXT:
            raise TransportError("unexpected binary message while waiting for catalog")
        summary = classify_catalog_reply(reply.payload)
        if summary is not None:
            return summary


def _prompt_credentials(username_option: Optional[str]) -> tuple[str, bytearray, str]:
    if not sys.stdin.isatty():
        raise CompanionError("physical mode requires an interactive TTY for no-echo prompts")
    username = username_option if username_option is not None else input("BLE username: ")
    # Validate before collecting either secret.  The password is validated when
    # the complete login command is built.
    _login_token(username, "username")

    secure_text = getpass.getpass("BLE Secure Channel secret: ")
    try:
        secure_secret = bytearray(secure_text.encode("utf-8", errors="strict"))
    finally:
        del secure_text
    if not secure_secret:
        _wipe(secure_secret)
        raise CompanionError("Secure Channel secret must not be empty")

    try:
        password = getpass.getpass("BLE login password: ")
    except BaseException:
        _wipe(secure_secret)
        raise
    return username, secure_secret, password


async def _run_connected_physical(
    args: argparse.Namespace,
    bleak_backend: Any,
    client_type: Any,
    device: Any,
    username: str,
    channel: SecureChannelV1Client,
    login_command: bytearray,
) -> CatalogSummary:
    loop = asyncio.get_running_loop()
    connection_token = object()
    generation: Optional[int] = None
    pump: Optional[_NotificationPump] = None
    client: Any = None
    primary_error: Optional[BaseException] = None
    cleanup_error: Optional[BaseException] = None
    notify_started = False
    try:
        generation, hello = channel.begin_handshake()
        pump = _NotificationPump(loop, connection_token, generation)
        client = client_type(
            device,
            disconnected_callback=pump.disconnected,
            timeout=args.connect_timeout,
        )
        try:
            await client.connect()
        except Exception as exc:
            raise TransportError("BLE connection failed") from exc
        if not client.is_connected:
            raise TransportError("BLE client did not reach connected state")
        request, response = _find_command_characteristics(client)
        await _require_secure_mtu(client, bleak_backend)
        try:
            await client.start_notify(response, pump.callback)
            notify_started = True
        except Exception as exc:
            raise TransportError("could not subscribe to command responses") from exc

        await _establish_channel(
            client,
            request,
            pump,
            channel,
            generation,
            hello,
            args.handshake_timeout,
        )
        print("Secure Channel v1 established; credentials were not logged.")

        await _send_encrypted_command(
            client, request, channel, generation, login_command
        )
        await _wait_for_login(
            pump, channel, generation, username, args.reply_timeout
        )
        print("Named BLE login accepted.")

        catalog_command = bytearray(CATALOG_COMMAND)
        await _send_encrypted_command(
            client, request, channel, generation, catalog_command
        )
        summary = await _wait_for_catalog(
            pump, channel, generation, args.reply_timeout
        )
        channel.assert_complete(generation)
        return summary
    except BaseException as exc:
        primary_error = exc
        raise
    finally:
        # This outer cleanup covers connect, MTU, notification-subscription,
        # and handshake failures that occur before _send_encrypted_command's
        # immediate wipe.
        _wipe(login_command)
        if pump is not None:
            pump.deactivate()
        connected = False
        if client is not None:
            try:
                connected = bool(client.is_connected)
            except Exception:
                connected = False
        if notify_started and connected:
            try:
                await client.stop_notify(COMMAND_RESPONSE_UUID)
            except Exception:
                pass
        if connected:
            try:
                await client.disconnect()
            except Exception as exc:
                cleanup_error = TransportError("BLE disconnect cleanup failed")
                cleanup_error.__cause__ = exc
        if channel is not None and generation is not None:
            try:
                channel.reset(generation)
            except IncompleteMessageError as exc:
                cleanup_error = exc
        if primary_error is None and cleanup_error is not None:
            raise cleanup_error


async def _run_physical(args: argparse.Namespace) -> CatalogSummary:
    bleak_backend, client_type, scanner_type = _load_bleak()
    # Confirm that exactly one phone-server advertisement exists before asking
    # the operator for either secret.  Scanning remains read-only and never
    # attempts the role-changing `openble` command.
    device = await _scan_phone_server(
        scanner_type,
        device_name=args.name,
        address=args.address,
        timeout=args.scan_timeout,
    )

    username, secure_secret, password = _prompt_credentials(args.username)
    login_command: Optional[bytearray] = None
    try:
        try:
            channel = SecureChannelV1Client(
                secure_secret, reassembly_timeout=args.reassembly_timeout
            )
        finally:
            _wipe(secure_secret)
        login_command = build_login_command(username, password)
        return await _run_connected_physical(
            args,
            bleak_backend,
            client_type,
            device,
            username,
            channel,
            login_command,
        )
    finally:
        # Double-wiping after _run_connected_physical is intentional.  This is
        # also the path for construction failures before any encrypted write.
        _wipe(secure_secret)
        _wipe(login_command)
        # getpass returns immutable Python strings, so deletion is the only
        # available lifetime reduction for that original object.
        del password


def _run_offline() -> CatalogSummary:
    _, compact, expected = _load_expected_catalog()
    actual = validate_live_catalog(compact)
    if actual != expected:
        raise CatalogValidationError("offline catalog fixture validation disagreed")
    return actual


def _positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a number") from exc
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be positive")
    return parsed


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Offline or explicitly physical Secure BLE event-catalog verifier"
    )
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument(
        "--offline",
        action="store_true",
        help="validate local vectors/fixture only (the default; never scans or connects)",
    )
    mode.add_argument(
        "--physical",
        action="store_true",
        help="explicitly scan/connect and verify one already-advertising device",
    )
    parser.add_argument("--name", default=DEFAULT_DEVICE_NAME, help="exact BLE local name")
    parser.add_argument("--address", help="exact BLE address/identifier (recommended if names collide)")
    parser.add_argument("--username", help="named login username (password remains prompt-only)")
    parser.add_argument("--scan-timeout", type=_positive_float, default=8.0)
    parser.add_argument("--connect-timeout", type=_positive_float, default=10.0)
    parser.add_argument("--handshake-timeout", type=_positive_float, default=8.0)
    parser.add_argument("--reply-timeout", type=_positive_float, default=15.0)
    parser.add_argument("--reassembly-timeout", type=_positive_float, default=2.0)
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    try:
        if args.physical:
            summary = asyncio.run(_run_physical(args))
            mode = "physical Secure BLE"
        else:
            summary = _run_offline()
            mode = "offline"
        print(
            f"PASS ({mode}): {summary.family_count} families, "
            f"{summary.kind_count} kinds, {summary.byte_count} compact JSON bytes"
        )
        return 0
    except (CompanionError, SecureChannelError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("FAIL: interrupted; BLE cleanup requested", file=sys.stderr)
        return 130
    except Exception as exc:
        # Do not print arbitrary transport exception strings: a backend should
        # never receive secrets, but this keeps the failure path non-logging by
        # construction if a future backend includes write bytes in an error.
        print(
            f"FAIL: unexpected {type(exc).__name__}; Secure BLE verification did not complete",
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
