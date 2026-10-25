"""The real radio: a CoreBluetooth peripheral serving the pipe service (pyobjc).

This is the only module that touches Bluetooth, and it is deliberately thin: it turns
``CBPeripheralManagerDelegate`` callbacks into :class:`hw1peer.gatt.PeripheralEvents` calls
and offers :class:`hw1peer.gatt.Peripheral`. Everything above it is tested offline with
``tests/fake_gatt.py``.

Rules this module follows:

* Everything runs on the MAIN thread. The manager is created with ``queue=None``, so
  CoreBluetooth delivers its callbacks on the main dispatch queue, which the main run loop
  serves (:func:`run_main_loop`). Never call into this module from another thread.
* The service is added once the radio is powered on and never touched while a live link uses
  it (adding or removing a service makes the P4 see Service Changed and end its session,
  ``BLE_PhoneGatt.cpp``). :meth:`MacPeripheral.reset_service` does exactly that on purpose:
  to retry a refused add, and after our side ended a link the P4 still holds.
* Advertising carries the 128-bit service UUID (``CBAdvertisementDataServiceUUIDsKey``),
  which CoreBluetooth puts in the primary advertisement, where the P4's passive scan looks.
* A peripheral gets no disconnect callback: ``didUnsubscribeFromCharacteristic`` is the
  signal (CoreBluetooth sends it when a subscribed central goes), and the app keeps its own
  watchdogs on top.
* ``updateValue:forCharacteristic:onSubscribedCentrals:`` returning NO means the stack's queue
  is full: the caller keeps the value and retries on ``peripheralManagerIsReadyToUpdateSubscribers``.
* No exception may cross into Objective-C: every callback is guarded and reported.
* Centrals are named by short keys ("c1", ...); their CoreBluetooth identifiers never leave
  this module, so no identifier reaches a log.

macOS only lets an app that holds the Bluetooth permission use CoreBluetooth (TCC). Run the
tool from Terminal.app and allow Terminal in System Settings > Privacy & Security > Bluetooth.
"""

import traceback
from collections.abc import Callable
from typing import Any

import CoreBluetooth as CB
import objc
from Foundation import (NSActivityLatencyCritical, NSActivityUserInitiated, NSData, NSDate, NSDefaultRunLoopMode,
                        NSObject, NSProcessInfo, NSRunLoop, NSTimer)

from hw1peer.gatt import (DEFAULT_LOCAL_NAME, RX_UUID, SERVICE_UUID, TX_UUID, AttError, NotifyResult,
                          PeripheralEvents, RadioState, WriteRequest, primary_name, triage_writes)

_STATES: dict[int, RadioState] = {
    CB.CBManagerStateUnknown: RadioState.UNKNOWN,
    CB.CBManagerStateResetting: RadioState.RESETTING,
    CB.CBManagerStateUnsupported: RadioState.UNSUPPORTED,
    CB.CBManagerStateUnauthorized: RadioState.UNAUTHORIZED,
    CB.CBManagerStatePoweredOff: RadioState.POWERED_OFF,
    CB.CBManagerStatePoweredOn: RadioState.POWERED_ON,
}


def _uuid(text: str) -> Any:
    return CB.CBUUID.UUIDWithString_(text)


def _uuid_text(cb_uuid: Any) -> str:
    """A CBUUID as a lowercase string (128-bit form for our UUIDs)."""
    return str(cb_uuid.UUIDString()).lower()


def _describe(error: Any) -> str:
    """An NSError as a short text (no addresses: CoreBluetooth errors carry none)."""
    try:
        return str(error.localizedDescription())
    except Exception:  # noqa: BLE001 - only for display
        return "unknown error"


class HW1PeerPeripheralDelegate(NSObject, protocols=[objc.protocolNamed("CBPeripheralManagerDelegate")]):
    """Forwards every delegate call to its :class:`MacPeripheral`, catching exceptions.

    The name is global in the Objective-C runtime, so it is specific to this tool."""

    def initWithOwner_(self, owner: "MacPeripheral") -> "HW1PeerPeripheralDelegate | None":
        self = objc.super(HW1PeerPeripheralDelegate, self).init()
        if self is None:
            return None
        self.owner = owner
        return self

    @objc.python_method
    def _guard(self, what: str, call: Callable[[], None]) -> None:
        try:
            call()
        except Exception:  # noqa: BLE001 - never let an exception reach Objective-C
            self.owner.report_error(f"{what} failed:\n{traceback.format_exc()}")

    def peripheralManagerDidUpdateState_(self, manager: Any) -> None:
        self._guard("state update", lambda: self.owner.radio_changed())

    def peripheralManager_didAddService_error_(self, manager: Any, service: Any, error: Any) -> None:
        self._guard("add service", lambda: self.owner.service_added(error))

    def peripheralManagerDidStartAdvertising_error_(self, manager: Any, error: Any) -> None:
        self._guard("start advertising", lambda: self.owner.advertising_started(error))

    def peripheralManager_central_didSubscribeToCharacteristic_(self, manager: Any, central: Any,
                                                               characteristic: Any) -> None:
        self._guard("subscribe", lambda: self.owner.subscribed(central, characteristic))

    def peripheralManager_central_didUnsubscribeFromCharacteristic_(self, manager: Any, central: Any,
                                                                   characteristic: Any) -> None:
        self._guard("unsubscribe", lambda: self.owner.unsubscribed(central, characteristic))

    def peripheralManager_didReceiveWriteRequests_(self, manager: Any, requests: Any) -> None:
        self._guard("write", lambda: self.owner.writes(requests))

    def peripheralManager_didReceiveReadRequest_(self, manager: Any, request: Any) -> None:
        self._guard("read", lambda: self.owner.read(request))

    def peripheralManagerIsReadyToUpdateSubscribers_(self, manager: Any) -> None:
        self._guard("ready to update", lambda: self.owner.ready_to_update())


class MacPeripheral:
    """:class:`hw1peer.gatt.Peripheral` on CoreBluetooth. Main thread only.

    ``name`` is the advertised local name (None or "" for none); ``on_error`` receives a text
    for any exception raised while handling a CoreBluetooth callback.
    """

    def __init__(self, *, name: str | None = DEFAULT_LOCAL_NAME,
                 on_error: Callable[[str], None] = print) -> None:
        self._name = primary_name(name)
        self._on_error = on_error
        self._events: PeripheralEvents | None = None
        self._manager: Any = None
        self._delegate: Any = None
        self._tx: Any = None
        self._service_added = False
        self._service_pending = False
        self._want_advertising = False
        self._advertising_requested = False
        self._keys: dict[str, str] = {}      # CoreBluetooth identifier -> short key
        self._centrals: dict[str, Any] = {}  # short key -> subscribed CBCentral

    # ---- gatt.Peripheral ------------------------------------------------------------------

    def start(self, events: PeripheralEvents) -> None:
        self._events = events
        self._delegate = HW1PeerPeripheralDelegate.alloc().initWithOwner_(self)
        options = {CB.CBPeripheralManagerOptionShowPowerAlertKey: True}
        # queue=None: callbacks on the main queue, served by the main run loop.
        self._manager = CB.CBPeripheralManager.alloc().initWithDelegate_queue_options_(self._delegate, None, options)

    def set_advertising(self, on: bool) -> None:
        self._want_advertising = on
        self._apply_advertising()

    def notify(self, central: str, value: bytes) -> NotifyResult:
        target = self._centrals.get(central)
        if target is None or self._tx is None or not self._powered():
            return NotifyResult.GONE
        data = NSData.dataWithBytes_length_(value, len(value))
        sent = self._manager.updateValue_forCharacteristic_onSubscribedCentrals_(data, self._tx, [target])
        return NotifyResult.SENT if sent else NotifyResult.BUSY

    def reset_service(self) -> None:
        """Remove the service and add it again (see :meth:`hw1peer.gatt.Peripheral.reset_service`)."""
        if self._manager is None or not self._powered() or self._service_pending:
            return
        if self._advertising_requested:
            self._manager.stopAdvertising()
            self._advertising_requested = False
            self._emit().on_advertising(False, None)
        self._service_added = False
        self._add_service()  # removeAllServices, then addService: didAddService follows

    def close(self) -> None:
        """Stop advertising and remove the service. Safe to call more than once."""
        self._want_advertising = False
        if self._manager is None:
            return
        if self._advertising_requested:
            self._manager.stopAdvertising()
            self._advertising_requested = False
        if self._service_added or self._service_pending:
            self._manager.removeAllServices()
        self._service_added = self._service_pending = False
        self._centrals.clear()
        self._tx = None

    # ---- delegate entry points (called by _Delegate) -------------------------------------

    def report_error(self, text: str) -> None:
        self._on_error(text)

    def radio_changed(self) -> None:
        state = _STATES.get(int(self._manager.state()), RadioState.UNKNOWN)
        if state is not RadioState.POWERED_ON:
            # The stack drops our service, advertising and subscriptions; no unsubscribe follows.
            lost = list(self._centrals)
            self._centrals.clear()
            self._service_added = self._service_pending = False
            self._advertising_requested = False
            self._tx = None
            for key in lost:
                self._emit().on_unsubscribe(key)
        self._emit().on_radio(state)
        if state is RadioState.POWERED_ON and not self._service_added and not self._service_pending:
            self._add_service()

    def service_added(self, error: Any) -> None:
        self._service_pending = False
        if error is not None:
            self._tx = None
            self._emit().on_service_added(_describe(error))
            return
        self._service_added = True
        self._emit().on_service_added(None)
        self._apply_advertising()

    def advertising_started(self, error: Any) -> None:
        if error is not None:
            self._advertising_requested = False
            self._emit().on_advertising(False, _describe(error))
            return
        if not self._advertising_requested:  # stopped before the start completed
            self._manager.stopAdvertising()
            return
        self._emit().on_advertising(True, None)

    def subscribed(self, central: Any, characteristic: Any) -> None:
        if _uuid_text(characteristic.UUID()) != TX_UUID:
            return
        key = self._key(central)
        self._centrals[key] = central
        self._emit().on_subscribe(key, int(central.maximumUpdateValueLength()))

    def unsubscribed(self, central: Any, characteristic: Any) -> None:
        if _uuid_text(characteristic.UUID()) != TX_UUID:
            return
        key = self._key(central)
        self._centrals.pop(key, None)
        self._emit().on_unsubscribe(key)

    def writes(self, requests: Any) -> None:
        batch = [WriteRequest(self._key(r.central()), _uuid_text(r.characteristic().UUID()), int(r.offset()),
                              bytes(r.value()) if r.value() is not None else b"")
                 for r in requests]
        status, accepted = triage_writes(batch)
        if batch:
            # One answer per batch, with the first request (ignored for write commands).
            self._manager.respondToRequest_withResult_(requests[0], int(status))
        for request in accepted:
            self._emit().on_write(request.central, request.value)

    def read(self, request: Any) -> None:
        self._manager.respondToRequest_withResult_(request, int(AttError.REQUEST_NOT_SUPPORTED))

    def ready_to_update(self) -> None:
        self._emit().on_ready_to_update()

    # ---- internals ------------------------------------------------------------------------

    def _emit(self) -> PeripheralEvents:
        assert self._events is not None, "start() first"
        return self._events

    def _powered(self) -> bool:
        return self._manager is not None and int(self._manager.state()) == CB.CBManagerStatePoweredOn

    def _key(self, central: Any) -> str:
        identifier = str(central.identifier().UUIDString())
        if identifier not in self._keys:
            self._keys[identifier] = f"c{len(self._keys) + 1}"
        return self._keys[identifier]

    def _add_service(self) -> None:
        rx = CB.CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
            _uuid(RX_UUID),
            CB.CBCharacteristicPropertyWrite | CB.CBCharacteristicPropertyWriteWithoutResponse,
            None, CB.CBAttributePermissionsWriteable)
        # Notify only. A read of TX is refused (no Read property; the delegate also answers 0x06).
        tx = CB.CBMutableCharacteristic.alloc().initWithType_properties_value_permissions_(
            _uuid(TX_UUID), CB.CBCharacteristicPropertyNotify, None, CB.CBAttributePermissionsReadable)
        service = CB.CBMutableService.alloc().initWithType_primary_(_uuid(SERVICE_UUID), True)
        service.setCharacteristics_([rx, tx])
        self._tx = tx
        self._service_pending = True
        # A clean table: the P4 refuses a second instance of the service (BLE_PhoneGatt.cpp
        # counts exactly one). Nothing is subscribed here, so nobody sees Service Changed.
        self._manager.removeAllServices()
        self._manager.addService_(service)

    def _apply_advertising(self) -> None:
        if self._manager is None:
            return
        want = self._want_advertising and self._powered() and self._service_added
        if want and not self._advertising_requested:
            data = {CB.CBAdvertisementDataServiceUUIDsKey: [_uuid(SERVICE_UUID)]}
            if self._name:
                data[CB.CBAdvertisementDataLocalNameKey] = self._name
            self._advertising_requested = True
            self._manager.startAdvertising_(data)
        elif not want and self._advertising_requested:
            self._manager.stopAdvertising()
            self._advertising_requested = False
            self._emit().on_advertising(False, None)


def hold_awake(reason: str) -> Callable[[], None]:
    """Keep the Mac awake and the tick on time for the whole run; returns the function that
    ends it. An NSProcessInfo activity: user-initiated (idle system sleep disabled, so a long
    idle test does not drop the radio) and latency-critical (no timer coalescing of the 10 ms
    tick). macOS shows it in ``pmset -g assertions`` as PreventUserIdleSystemSleep. It does
    not prevent the sleep of a closed lid: keep the Mac open (and on power) during a test."""
    info = NSProcessInfo.processInfo()
    token = info.beginActivityWithOptions_reason_(NSActivityUserInitiated | NSActivityLatencyCritical, reason)
    ended = False

    def end() -> None:
        nonlocal ended
        if not ended:
            ended = True
            info.endActivity_(token)
    return end


def run_main_loop(tick: Callable[[], None], should_stop: Callable[[], bool], period_s: float = 0.010) -> None:
    """Serve the main run loop (CoreBluetooth's callbacks) and call ``tick`` every
    ``period_s`` from an NSTimer, until ``should_stop()`` is true. An exception from ``tick``
    stops the loop and is re-raised here (never inside Objective-C)."""
    failures: list[BaseException] = []

    def fire(_timer: Any) -> None:
        if failures:
            return
        try:
            tick()
        except BaseException as error:  # noqa: BLE001 - carried out of the run loop
            failures.append(error)

    timer = NSTimer.scheduledTimerWithTimeInterval_repeats_block_(period_s, True, fire)
    loop = NSRunLoop.currentRunLoop()
    try:
        while not failures and not should_stop():
            # Returns after an input source or at the date; timers fire meanwhile.
            loop.runMode_beforeDate_(NSDefaultRunLoopMode, NSDate.dateWithTimeIntervalSinceNow_(0.1))
    finally:
        timer.invalidate()
    if failures:
        raise failures[0]
