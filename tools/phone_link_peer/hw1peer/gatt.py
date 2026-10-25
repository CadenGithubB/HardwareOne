"""The GATT boundary: what the app needs from a BLE peripheral, and what it hears back.

The P4 is the GATT client (central) and this Mac is the server (peripheral), exactly as
the Android app (``PipeGattServer.kt``) and the firmware's ``BLE_PhoneGatt.cpp`` expect:

* one primary service ``b7e10000-6f7d-4b2e-a8c4-3d9f5021c6a0``;
* RX ``b7e10001-…``: the P4 writes to it without response (P4 -> phone values);
* TX ``b7e10002-…``: notify only; the P4's CCCD write (``01 00``) is the subscription that
  starts the secure handshake, and every phone -> P4 value is a notification;
* connectable advertising with the 128-bit service UUID in the PRIMARY advertising data:
  the P4 scans passively and only reads the legacy advertisement (``hasService`` in
  ``BLE_PhonePipe.cpp`` looks for AD type 0x06/0x07 and refuses a payload over 31 bytes).

:class:`Peripheral` is implemented by ``gatt_macos.MacPeripheral`` (CoreBluetooth, real
radio) and by ``tests/fake_gatt.FakePeripheral`` (offline). Everything above this boundary is
plain Python and runs offline. Both sides of the boundary are called on ONE thread (the main
thread's run loop on macOS); nothing here is thread-safe.

Centrals are named by short opaque keys ("c1", "c2", ...) chosen by the peripheral, never by
an address or a CoreBluetooth identifier, so logs never carry one.
"""

from collections.abc import Sequence
from dataclasses import dataclass, field
from enum import Enum, IntEnum
from typing import Final, Protocol

SERVICE_UUID: Final = "b7e10000-6f7d-4b2e-a8c4-3d9f5021c6a0"   # hw1pipe::kServiceUuid
RX_UUID: Final = "b7e10001-6f7d-4b2e-a8c4-3d9f5021c6a0"        # P4 -> phone, write without response
TX_UUID: Final = "b7e10002-6f7d-4b2e-a8c4-3d9f5021c6a0"        # phone -> P4, notify

#: CoreBluetooth gives a foreground app 28 bytes of advertising data: the 128-bit UUID takes
#: 18 (length, type, 16 bytes), so a local name of at most 8 bytes still fits beside it.
MAX_PRIMARY_NAME: Final = 8
DEFAULT_LOCAL_NAME: Final = "HW1Peer"


class AttError(IntEnum):
    """The ATT status codes this server answers with (CBATTError values)."""

    SUCCESS = 0x00
    WRITE_NOT_PERMITTED = 0x03
    REQUEST_NOT_SUPPORTED = 0x06
    INVALID_OFFSET = 0x07
    INVALID_LENGTH = 0x0D
    UNLIKELY = 0x0E


class RadioState(Enum):
    """CBManagerState, by name."""

    UNKNOWN = "unknown"
    RESETTING = "resetting"
    UNSUPPORTED = "unsupported"
    UNAUTHORIZED = "unauthorized"    # Terminal is not allowed to use Bluetooth (TCC)
    POWERED_OFF = "powered off"
    POWERED_ON = "powered on"


class NotifyResult(Enum):
    """What happened to one notification."""

    SENT = "sent"   # the stack took the value (CoreBluetooth's updateValue returned YES)
    BUSY = "busy"   # the stack's queue is full: keep the value, retry on on_ready_to_update()
    GONE = "gone"   # no such subscribed central (or the radio is down): the link is lost


@dataclass(frozen=True, slots=True)
class WriteRequest:
    """One ATT write as the peripheral saw it (CBATTRequest)."""

    central: str
    characteristic: str          # lowercase UUID string
    offset: int
    value: bytes = field(repr=False)


def triage_writes(requests: Sequence[WriteRequest]) -> tuple[AttError, list[WriteRequest]]:
    """The answer to one ``didReceiveWriteRequests`` batch and the values to deliver.

    CoreBluetooth wants exactly one response per batch, given with the FIRST request, and a
    batch is all-or-nothing: if any request is refused, none is applied. RX takes values at
    offset 0 only (there are no long writes in the pipe). Write commands (without response)
    arrive the same way; their response is simply ignored by the stack.
    """
    for request in requests:
        if request.characteristic != RX_UUID:
            return AttError.WRITE_NOT_PERMITTED, []
        if request.offset != 0:
            return AttError.INVALID_OFFSET, []
    return AttError.SUCCESS, list(requests)


def primary_name(name: str | None) -> str | None:
    """The local name to advertise, or None: absent, empty, or longer than
    :data:`MAX_PRIMARY_NAME` UTF-8 bytes. A name is sent only if it fits beside the 128-bit
    service UUID, so the UUID can never be pushed out of the primary advertisement."""
    if not name or len(name.encode("utf-8")) > MAX_PRIMARY_NAME:
        return None
    return name


class PeripheralEvents(Protocol):
    """What a :class:`Peripheral` reports. Called on the peripheral's (main) thread."""

    def on_radio(self, state: RadioState) -> None:
        """The Bluetooth state changed. Below POWERED_ON every subscription is gone."""

    def on_service_added(self, error: str | None) -> None:
        """The pipe service was added (None) or refused (a short reason)."""

    def on_advertising(self, started: bool, error: str | None) -> None:
        """Advertising started or stopped (``error`` when a start failed)."""

    def on_subscribe(self, central: str, max_update: int) -> None:
        """The central enabled TX notifications (its CCCD write ``01 00``): the pipe's start.
        ``max_update`` is the central's ``maximumUpdateValueLength`` (ATT_MTU - 3)."""

    def on_unsubscribe(self, central: str) -> None:
        """The central disabled notifications or disconnected (CoreBluetooth reports a
        peripheral-side disconnect only this way)."""

    def on_write(self, central: str, value: bytes) -> None:
        """One RX value from the central, in order."""

    def on_ready_to_update(self) -> None:
        """The stack has room again after a :attr:`NotifyResult.BUSY`."""


class Peripheral(Protocol):
    """A BLE peripheral serving the pipe service (one subscribed central at a time is used)."""

    def start(self, events: PeripheralEvents) -> None:
        """Bring the radio up and add the service once it is powered on."""

    def set_advertising(self, on: bool) -> None:
        """Start or stop connectable advertising of the service UUID (only once the service
        is added; a request before that is remembered)."""

    def notify(self, central: str, value: bytes) -> NotifyResult:
        """Send one TX notification to that central only."""

    def reset_service(self) -> None:
        """Remove the pipe service and add it again (``on_service_added`` follows; advertising
        stops meanwhile). Used for two things only: to retry an add the stack refused, and after
        our side ended a link the P4 still holds, so that it sees Service Changed and stops at
        once (``BLE_PhoneGatt.cpp``: SRVC_CHG -> stop(Protocol)). Never while a live link uses
        the service. Ignored while the radio is off or an add is in flight."""

    def close(self) -> None:
        """Stop advertising and remove the service (the subscribed central sees it go)."""
