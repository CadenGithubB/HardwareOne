"""A fake GATT peripheral and a loopback bench for the app's tests.

:class:`FakePeripheral` implements :class:`hw1peer.gatt.Peripheral` the way CoreBluetooth
behaves for the app (one main thread, events delivered synchronously): the service is added
when "powered on", advertising is a flag, notifications go to the subscribed central's sink,
and a scripted budget of BUSY answers models ``updateValue`` returning NO until
:meth:`FakePeripheral.ready` (``peripheralManagerIsReadyToUpdateSubscribers``). A scripted
budget of refused service adds models ``didAddService`` with an error, and
:meth:`FakePeripheral.reset_service` sends every subscribed central a Service Changed
indication (the P4 model stops on it, as ``BLE_PhoneGatt.cpp`` does). It also checks
what the real stack would not allow: a notification longer than ATT_MTU - 3 (CoreBluetooth
would truncate it) or to a central that is not subscribed.

On the central side it offers the calls :class:`~hw1peer.tests.p4_model.P4Gatt` makes:
connect (with an MTU), subscribe (the CCCD write), write without response, disconnect.

:class:`Bench` wires a :class:`~hw1peer.app.Peer` (the full app stack: channel, inner,
records, relay) to a P4 model over the fake, on a millisecond test clock: the P4 polls every
millisecond, the app ticks every 10 ms, as on the devices.
"""

import io
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from hw1peer import paircode
from hw1peer.app import Credentials, Output, Peer, Request, RequestKind, Settings, credentials_from_entry
from hw1peer.gatt import RX_UUID, NotifyResult, PeripheralEvents, RadioState, WriteRequest, triage_writes
from hw1peer.relay import TcpRelay
from hw1peer.tests.p4_model import P4Gatt, default_responder


@dataclass
class _Central:
    mtu: int
    sink: Callable[[bytes], None]
    subscribed: bool = False
    on_service_changed: Callable[[], None] | None = None


class FakePeripheral:
    """:class:`hw1peer.gatt.Peripheral` for tests (see the module docstring)."""

    def __init__(self) -> None:
        self.events: PeripheralEvents | None = None
        self.advertising = False
        self.advertising_calls: list[bool] = []
        self.closed = False
        self.busy_budget = 0       # the next N notifications answer BUSY
        self.busy_answers = 0
        self.notifications: list[tuple[str, bytes]] = []
        self.add_failures = 0      # the next N service adds are refused (didAddService with an error)
        self.service_adds = 0      # every add, refused or not
        self.service_resets = 0    # reset_service calls that removed and re-added the service
        self.powered = False
        self._centrals: dict[str, _Central] = {}

    def _ev(self) -> PeripheralEvents:
        assert self.events is not None, "start() first"
        return self.events

    # ---- gatt.Peripheral --------------------------------------------------------------------

    def start(self, events: PeripheralEvents) -> None:
        self.events = events
        self.powered = True
        events.on_radio(RadioState.POWERED_ON)
        self._add_service()

    def _add_service(self) -> None:
        self.service_adds += 1
        if self.add_failures > 0:
            self.add_failures -= 1
            self._ev().on_service_added("Unknown error.")
        else:
            self._ev().on_service_added(None)

    def set_advertising(self, on: bool) -> None:
        self.advertising_calls.append(on)
        if on != self.advertising:
            self.advertising = on
            self._ev().on_advertising(on, None)

    def notify(self, central: str, value: bytes) -> NotifyResult:
        target = self._centrals.get(central)
        if target is None or not target.subscribed or self.closed:
            return NotifyResult.GONE
        if self.busy_budget > 0:
            self.busy_budget -= 1
            self.busy_answers += 1
            return NotifyResult.BUSY
        assert len(value) <= target.mtu - 3, "CoreBluetooth would truncate this notification"
        self.notifications.append((central, value))
        target.sink(value)
        return NotifyResult.SENT

    def reset_service(self) -> None:
        """Remove and re-add the service: subscribed centrals get Service Changed."""
        if not self.powered or self.closed:
            return
        self.service_resets += 1
        if self.advertising:
            self.advertising = False
            self._ev().on_advertising(False, None)
        for central in self._centrals.values():
            if central.subscribed and central.on_service_changed is not None:
                central.on_service_changed()
        self._add_service()

    def close(self) -> None:
        self.closed = True
        self.advertising = False

    # ---- the central's side (FakeRadio for P4Gatt) -------------------------------------------

    def connect(self, key: str, mtu: int, sink: Callable[[bytes], None],
                on_service_changed: Callable[[], None] | None = None) -> None:
        self._centrals[key] = _Central(mtu, sink, on_service_changed=on_service_changed)

    def subscribe(self, key: str) -> None:
        central = self._centrals[key]
        central.subscribed = True
        self._ev().on_subscribe(key, central.mtu - 3)

    def write(self, key: str, value: bytes) -> None:
        status, accepted = triage_writes([WriteRequest(key, RX_UUID, 0, value)])
        assert status == 0
        for request in accepted:
            self._ev().on_write(request.central, request.value)

    def disconnect(self, key: str) -> None:
        central = self._centrals.pop(key, None)
        if central is not None and central.subscribed:
            self._ev().on_unsubscribe(key)

    def ready(self) -> None:
        """peripheralManagerIsReadyToUpdateSubscribers."""
        self._ev().on_ready_to_update()

    def radio(self, state: RadioState) -> None:
        """The radio changed (subscriptions die below POWERED_ON, as in gatt_macos)."""
        self.powered = state is RadioState.POWERED_ON
        if state is not RadioState.POWERED_ON:
            for key, central in list(self._centrals.items()):
                if central.subscribed:
                    central.subscribed = False
                    self._ev().on_unsubscribe(key)
            self.advertising = False
        self._ev().on_radio(state)
        if state is RadioState.POWERED_ON:
            self._add_service()


def fixed_random(seed: int = 1) -> Callable[[int], bytes]:
    """A deterministic random source (a counter-based stream; never all zero)."""
    counter = [seed]

    def draw(size: int) -> bytes:
        out = bytearray()
        while len(out) < size:
            counter[0] += 1
            out += (counter[0] * 0x9E3779B97F4A7C15 & (1 << 64) - 1).to_bytes(8, "little")
        return bytes(out[:size])
    return draw


#: A valid pairing code (made from fixed values; synthetic, never a real secret).
CODE = paircode.from_values(list(range(1, 16)))
OTHER_CODE = paircode.from_values(list(range(2, 17)))


class Bench:
    """The app (Peer) and a P4 model over a FakePeripheral, on a millisecond test clock."""

    def __init__(self, *, code: str | None = CODE, p4_code: str = CODE, mtu: int = 247,
                 settings: Settings | None = None, console_offered: bool = True,
                 responder: Callable[[str], str | None] = default_responder,
                 relay: TcpRelay | None = None, realtime: Callable[[], None] | None = None,
                 log: Path | None = None, add_failures: int = 0) -> None:
        self.now = 0
        self.screen = io.StringIO()
        self.out = Output(log, stream=self.screen)
        self.fake = FakePeripheral()
        self.fake.add_failures = add_failures
        self.relay = relay if relay is not None else TcpRelay(log=self.out.say)
        self.peer = Peer(self.fake, self.out, relay=self.relay, settings=settings or Settings(),
                         clock=lambda: self.now, random=fixed_random(7))
        self.peer.start()
        if code is not None:
            self.give_secret(code)
        self.p4 = P4Gatt(self.fake, paircode.psk(p4_code.encode("ascii")), mtu=mtu,
                         console_offered=console_offered, responder=responder, random=fixed_random(99))
        self.realtime = realtime   # called every simulated ms when real sockets are involved

    # ---- inputs ---------------------------------------------------------------------------------

    def give_secret(self, entry: str) -> None:
        creds = credentials_from_entry(entry)
        assert isinstance(creds, Credentials), creds
        self.peer.inbox.put(Request(RequestKind.SECRET, creds=creds))
        self.peer.tick()

    def command(self, line: str, source: str = "repl") -> None:
        self.peer.command(line, self.now, source=source)

    # ---- the clock ------------------------------------------------------------------------------

    def step(self, ms: int = 1) -> None:
        for _ in range(ms):
            self.now += 1
            self.p4.poll(self.now)
            if self.now % 10 == 0:
                self.peer.tick()
            if self.realtime is not None:
                self.realtime()

    def until(self, condition: Callable[[], bool], limit_ms: int = 10_000, what: str = "condition") -> int:
        """Step until ``condition()``; returns the milliseconds it took."""
        start = self.now
        while not condition():
            assert self.now - start < limit_ms, f"{what} not reached in {limit_ms} ms\n{self.text()}"
            self.step()
        return self.now - start

    def connect(self) -> None:
        """The P4 finds the advertisement and subscribes."""
        assert self.p4.connect(self.now), "the app is not advertising"

    def up(self) -> None:
        """Connect and wait for READY on both sides."""
        self.connect()
        self.until(lambda: self.p4.ready and self.peer.session() is not None and self.peer.session().ready,
                   what="READY")

    def console_up(self) -> None:
        self.up()
        self.until(lambda: self.peer.console.stream != 0 and self.peer.console.info is not None, what="console open")

    def close(self) -> None:
        """Quit the app side at once (closes relay sockets and the log)."""
        self.peer.close()
        self.out.close()

    # ---- output ---------------------------------------------------------------------------------

    def text(self) -> str:
        return self.screen.getvalue()

    def lines(self) -> list[str]:
        return self.text().splitlines()
