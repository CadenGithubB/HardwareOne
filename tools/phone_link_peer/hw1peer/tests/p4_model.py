"""A Python model of the P4 for the app's loopback tests, built from the firmware.

* :class:`P4Session` is a line-by-line port of ``hw1pipe::Session`` (``BLE_PhonePipeCore.h``):
  the P4's inner protocol v2 with its validation, slots, credit, control queue, LINK_STATE,
  CLOSE/closeAfterDrain, PING and the version-signal acceptance, and the pressure inputs of
  ``tick``: ``relay_pressure`` defers a relay stream's DATA/CREDIT within its cumulative 2 s
  budget, ``console_display_busy`` defers the console's for as long as it lasts
  (``Stream::deferred()``, the tick budget and the skip-deferred-CREDIT step of ``peek``).
* :class:`P4Gatt` follows ``Gatt::consume``/``Gatt::poll`` (``BLE_PhoneGatt.cpp``): it
  subscribes (the CCCD write), answers the phone's secure handshake with the firmware's own
  responder (:class:`hw1peer.channel.DeviceChannel`), refuses a bad CONFIRM with ``05 02``,
  starts the pipe with a random nonce at the negotiated MTU, writes one value per 30 ms, checks
  every notification's length against valueLimit(MTU), stops with BAD_PROTOCOL on a Service
  Changed indication (``ESP_GATTC_SRVC_CHG_EVT``), and disconnects on any failure.
* :class:`P4Console` is a reduced ``BLE_PhonePipeConsole.cpp``: it opens the console when
  READY && CONSOLE_OFFERED && CONSOLE_WANTED && slot 2 is free and no hold is in force, writes
  INFO at OPEN_OK, answers COMMAND records through a responder with TEXT chains, closes with
  3/4 when the offer or the phone's wish falls, and applies the reopen holds of §5.3 (the
  backoff ladder 2/5/10 s, the wanted-rise wait, "never again" after 17).

The model talks to the app only through ``tests/fake_gatt.FakePeripheral``, exactly the
calls a real central makes: subscribe, write without response, disconnect.
"""

import os
from collections import deque
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum

from hw1peer import records
from hw1peer.channel import DeviceChannel
from hw1peer.inner import Error, Op

HEADER, VALUE, CONTROL, WINDOW = 12, 512, 272, 4096
FIRST, LAST = 1, 2
VERSION, MAX_STREAMS = 2, 3
RELAY_SLOTS, CONSOLE_SLOT, CONTROL_QUEUE = 2, 2, 12
RELAY_CREDIT, CONSOLE_CREDIT = 1024, 4096
P4_CONSOLE_OFFERED, PHONE_RELAY_ON, PHONE_CONSOLE_WANTED = 0x01, 0x01, 0x02
P4_FLAG_MASK, PHONE_FLAG_MASK = 0x01, 0x03
U32_MAX = 0xFFFF_FFFF
SECURE_OVERHEAD = 25
PRESSURE_BUDGET_MS = 2000


def value_limit(mtu: int) -> int:
    return 0 if mtu < 23 else (512 if mtu > 515 else mtu - 3)


def expired(now: int, start: int, duration: int) -> bool:
    return now - start >= duration


def known_error(code: int) -> bool:
    return 1 <= code <= 18


def get16(b: bytes, at: int = 0) -> int:
    return b[at] | b[at + 1] << 8


def get32(b: bytes, at: int = 0) -> int:
    return get16(b, at) | get16(b, at + 2) << 16


def le16(v: int) -> bytes:
    return v.to_bytes(2, "little")


def le32(v: int) -> bytes:
    return v.to_bytes(4, "little")


class SState(Enum):
    DOWN = "down"
    HELLO = "hello"
    READY = "ready"
    FAILED = "failed"


class StState(Enum):
    FREE = "free"
    OPENING = "opening"
    OPEN = "open"
    CLOSING = "closing"
    ENDED = "ended"


class HelloEnd(Enum):
    NONE = "none"
    VERSION_SIGNAL = "version_signal"
    SILENT = "silent"


@dataclass(eq=False)
class Stream:
    console: bool
    id: int = 0
    state: StState = StState.FREE
    error: int = 0
    tx: bytearray = field(default_factory=bytearray)
    rx: bytearray = field(default_factory=bytearray)
    sent: int = 0
    received: int = 0
    consumed: int = 0
    peer_limit: int = 0
    receive_limit: int = 0
    queued_limit: int = 0
    credit_at: int = 0
    close_at: int = 0
    opened_at: int = 0
    eof: bool = False
    open_submitted: bool = False
    remote_end: bool = False
    draining: bool = False
    drain_reason: int = 0
    drain_at: int = 0
    close_reason: int = 0
    peer_close_reason: int = 0
    # Relay: one cumulative 2 s pressure budget per stream; console: deferred while the
    # display is busy, without a budget (BLE_PhonePipeCore.h Stream).
    pressure_at: int = 0
    pressure_ms: int = 0
    pressure_active: bool = False

    def reset(self) -> None:
        console = self.console
        self.__init__(console)  # type: ignore[misc]

    def live(self) -> bool:
        return self.state in (StState.OPENING, StState.OPEN, StState.CLOSING)

    def deferred(self) -> bool:
        """``Stream::deferred()``: DATA and CREDIT wait while pressure lasts (relay: within budget)."""
        return self.pressure_active and (self.console or self.pressure_ms < PRESSURE_BUDGET_MS)


@dataclass(eq=False)
class Message:
    op: Op
    id: int
    body: bytes
    offset: int = 0


class P4Session:
    """``hw1pipe::Session``, P4 side (method names follow the C++)."""

    def __init__(self) -> None:
        self.state = SState.DOWN
        self.error = 0
        self.peer_reset = False
        self.streams = [Stream(i == CONSOLE_SLOT) for i in range(MAX_STREAMS)]
        self.value = 20
        self.tx_seq = 0
        self.rx_seq = 0
        self._messages: list[Message] = []
        self._round = self._relay_turns = 0
        self._next_id = 1
        self._nonce = bytes(8)
        self._packet = b""
        self._pending = self._pending_control = False
        self._assembling = False
        self._assembly = bytearray()
        self._assembly_op: Op | int = Op.RESET
        self._assembly_id = 0
        self._started = self._last_rx = self._last_tx = self._assembly_at = 0
        self._ping_cookie = self._ping_at = 0
        self._ping_pending = False
        self._local_flags = self._sent_flags = self._peer_flags = 0
        self._link_state_sent = False
        self._link_state_at = 0
        self._hello_accepted = False
        self.hello_end = HelloEnd.NONE
        self._hello_at = 0
        self.pings_sent = 0
        self.pongs_received = 0

    # ---- lifecycle ------------------------------------------------------------------------

    def down(self, why: int = Error.CONNECT_FAILED, now: int | None = None) -> None:
        if now is not None and why == Error.CONNECT_FAILED and self._hello_unanswered() and \
                not expired(now, self._hello_at, 1500):
            self.hello_end = HelloEnd.SILENT
        self._teardown(why)

    def start(self, mtu: int, nonce: bytes, now: int, outer_overhead: int = 0) -> bool:
        self.down()
        self.hello_end = HelloEnd.NONE
        self._hello_at = 0
        self._hello_accepted = False
        self.value = value_limit(mtu)
        if not self.value or len(nonce) != 8:
            return self._fail(Error.BAD_PROTOCOL)
        if self.value < outer_overhead + 20:
            return self._fail(Error.BAD_PROTOCOL)
        self.value -= outer_overhead
        if not any(nonce):
            return self._fail(Error.BAD_PROTOCOL)
        self._nonce = bytes(nonce)
        self.tx_seq = self.rx_seq = 0
        self._next_id = 1
        self.peer_reset = False
        for s in self.streams:
            s.reset()
        self.error = 0
        self.state = SState.HELLO
        self._started = self._last_rx = self._last_tx = now
        self._ping_pending = False
        self._round = self._relay_turns = 0
        self._peer_flags = 0
        self._sent_flags = self._local_flags
        self._link_state_sent = False
        body = self._nonce + bytes((MAX_STREAMS, self._local_flags)) + le16(self.value)
        return self._queue(Op.HELLO, 0, body)

    def set_local_flags(self, flags: int) -> None:
        self._local_flags = flags & P4_FLAG_MASK

    @property
    def local_flags(self) -> int:
        return self._local_flags

    def peer_flags(self) -> int:
        return self._peer_flags if self.state is SState.READY else 0

    def find(self, id_: int) -> Stream | None:
        return next((s for s in self.streams if id_ and s.id == id_), None)

    def release(self, id_: int) -> None:
        s = self.find(id_)
        if s is not None and not s.live():
            s.reset()

    # ---- opening ----------------------------------------------------------------------------

    def open(self, kind: int, host: bytes, port: int, now: int) -> int:
        if (self.state is not SState.READY or not port or not self._next_id or len(host) > 253 or
                not ((kind == 1 and len(host) == 4) or (kind == 2 and len(host) == 16) or (kind == 3 and host))):
            return 0
        if kind == 3 and not all(c in b"abcdefghijklmnopqrstuvwxyz0123456789-." for c in host):
            return 0
        s = next((self.streams[i] for i in range(RELAY_SLOTS) if self.streams[i].state is StState.FREE), None)
        if s is None:
            return 0
        s.reset()
        s.id, self._next_id = self._next_id, self._next_id + 1
        s.state = StState.OPENING
        s.receive_limit = s.queued_limit = RELAY_CREDIT
        s.credit_at = now
        body = le32(RELAY_CREDIT) + le16(port) + bytes((kind, len(host))) + bytes(host)
        if not self._queue(Op.OPEN, s.id, body):
            return 0
        return s.id

    def open_console(self, now: int) -> int:
        s = self.streams[CONSOLE_SLOT]
        if self.state is not SState.READY or not self._next_id or s.state is not StState.FREE:
            return 0
        s.reset()
        s.id, self._next_id = self._next_id, self._next_id + 1
        s.state = StState.OPENING
        s.receive_limit = s.queued_limit = CONSOLE_CREDIT
        s.credit_at = now
        if not self._queue(Op.OPEN, s.id, le32(CONSOLE_CREDIT) + le16(1) + bytes((4, 0))):
            return 0
        return s.id

    def relay_live(self) -> bool:
        return any(self.streams[i].live() for i in range(RELAY_SLOTS))

    # ---- stream bytes -------------------------------------------------------------------------

    def write(self, id_: int, data: bytes) -> int:
        s = self.find(id_)
        if self.state is not SState.READY or s is None or s.state is not StState.OPEN or s.draining:
            return 0
        n = min(len(data), WINDOW - len(s.tx))
        s.tx += data[:n]
        return n

    def read(self, id_: int, n: int = WINDOW) -> bytes:
        s = self.find(id_)
        if s is None or s.state is not StState.OPEN:
            return b""
        data = bytes(s.rx[:n])
        del s.rx[:len(data)]
        s.consumed += len(data)
        return data

    def rx_available(self, id_: int) -> int:
        s = self.find(id_)
        return len(s.rx) if s is not None and s.state is StState.OPEN else 0

    def rx_peek(self, id_: int, n: int) -> bytes:
        s = self.find(id_)
        return bytes(s.rx[:n]) if s is not None and s.state is StState.OPEN else b""

    def consume(self, id_: int, n: int) -> bool:
        s = self.find(id_)
        if s is None or s.state is not StState.OPEN or n > len(s.rx):
            return False
        del s.rx[:n]
        s.consumed += n
        return True

    def tx_space(self, id_: int) -> int:
        s = self.find(id_)
        ok = self.state is SState.READY and s is not None and s.state is StState.OPEN and not s.draining
        return WINDOW - len(s.tx) if ok and s is not None else 0

    def close(self, id_: int, now: int, cancelled: bool = False) -> bool:
        s = self.find(id_)
        if s is None or not s.live() or s.state is StState.CLOSING:
            return True
        return self._close_now(s, now, 1 if s.console else (1 if cancelled else 0))

    def close_after_drain(self, id_: int, now: int, reason: int) -> bool:
        s = self.find(id_)
        if s is None or not s.live() or s.state is StState.CLOSING or s.draining:
            return True
        if s.state is StState.OPENING or not s.tx:
            return self._close_now(s, now, reason)
        s.draining, s.drain_reason, s.drain_at = True, reason, now
        return True

    # ---- timers -------------------------------------------------------------------------------

    def tick(self, now: int, relay_pressure: bool = False, console_display_busy: bool = False) -> bool:
        """``Session::tick``. ``relay_pressure`` defers relay DATA/CREDIT within each stream's 2 s
        budget; ``console_display_busy`` defers console DATA/CREDIT for as long as it lasts.
        Control frames never yield."""
        if self.state is SState.HELLO and expired(now, self._started, 3000):
            if self._hello_unanswered():
                self.hello_end = HelloEnd.SILENT
            return self._fail(Error.DEADLINE)
        if self._assembling and expired(now, self._assembly_at, 2000):
            return self._fail(Error.DEADLINE)
        if self.state is not SState.READY:
            return self.state is not SState.FAILED
        if not self._link_state(now):
            return False
        for s in self.streams:
            if s.state is StState.CLOSING and expired(now, s.close_at, 2000):
                return self._fail(Error.DEADLINE)
            if s.state is not StState.OPEN:
                continue
            if s.console:
                s.pressure_active = console_display_busy
            else:
                if s.pressure_active:
                    elapsed = (now - s.pressure_at) & U32_MAX
                    s.pressure_ms += min(elapsed, PRESSURE_BUDGET_MS - s.pressure_ms)
                s.pressure_active = relay_pressure
            s.pressure_at = now
            if s.draining and (not s.tx or expired(now, s.drain_at, 3000)):
                if not self._close_now(s, now, s.drain_reason):
                    return False
                continue
            if s.eof or s.deferred():
                continue
            window = CONSOLE_CREDIT if s.console else RELAY_CREDIT
            if s.consumed > U32_MAX - window:
                return self._fail(Error.FLOW_VIOLATION)
            limit = s.consumed + window
            if limit > s.queued_limit and (limit - s.queued_limit >= 512 or expired(now, s.credit_at, 100)):
                if not self._queue(Op.CREDIT, s.id, le32(limit)):
                    return False
                s.queued_limit, s.credit_at = limit, now
        if self._ping_pending and expired(now, self._ping_at, 5000):
            return self._fail(Error.DEADLINE)
        if not self._ping_pending and ((self.relay_live() and expired(now, self._last_rx, 5000)) or
                                       expired(now, self._last_rx, 60000) or expired(now, self._last_tx, 60000)):
            cookie = now & U32_MAX
            self._ping_cookie, self._ping_at, self._ping_pending = cookie, now, True
            self.pings_sent += 1
            return self._queue(Op.PING, 0, le32(cookie))
        return True

    # ---- TX -----------------------------------------------------------------------------------

    def peek(self) -> bytes | None:
        if self.state not in (SState.READY, SState.HELLO):
            return None
        if self.tx_seq == U32_MAX:
            self._fail(Error.BAD_PROTOCOL)
            return None
        if self._pending:
            s = self.find(get16(self._packet, 2))
            if s is not None and s.deferred() and self._packet[1] in (Op.DATA, Op.CREDIT):
                self._unpend()  # a held DATA/CREDIT of a stream now under pressure goes back
            else:
                return self._packet
        # Deferred CREDIT cannot hold CLOSE/RESET/PING or another stream hostage: the first
        # message that is not a deferred stream's CREDIT moves to the front.
        selected = 0
        while selected < len(self._messages):
            m = self._messages[selected]
            s = self.find(m.id)
            if m.op != Op.CREDIT or s is None or not s.deferred():
                break
            selected += 1
        if selected < len(self._messages):
            if selected:
                self._messages.insert(0, self._messages.pop(selected))
            m = self._messages[0]
            n = min(len(m.body) - m.offset, self.value - HEADER)
            flags = (FIRST if m.offset == 0 else 0) | (LAST if m.offset + n == len(m.body) else 0)
            op, id_, body = m.op, m.id, m.body[m.offset:m.offset + n]
            self._pending_control = True
        else:
            s = self._next_data()
            if s is None:
                return None
            n = min(self.value - HEADER, s.peer_limit - s.sent)
            op, id_, body, flags = Op.DATA, s.id, bytes(s.tx[:n]), 0
            self._pending_control = False
        self._packet = (bytes((VERSION, op)) + le16(id_) + le32(self.tx_seq) + le16(len(body)) + bytes((flags, 0))
                        + body)
        self._pending = True
        return self._packet

    def accept(self, now: int) -> bool:
        if not self._pending:
            return False
        n = len(self._packet) - HEADER
        if self._pending_control:
            m = self._messages[0]
            m.offset += n
            if m.offset == len(m.body):
                if m.op is Op.HELLO:
                    self._hello_accepted, self._hello_at = True, now
                if m.op is Op.OPEN and (s := self.find(m.id)) is not None:
                    s.open_submitted = True
                if m.op is Op.CLOSE_ACK and (s := self.find(m.id)) is not None and s.live():
                    self._end(s, s.error)
                if m.op is Op.CREDIT and (s := self.find(m.id)) is not None and s.state is StState.OPEN:
                    s.receive_limit = get32(m.body)
                self._messages.pop(0)
        else:
            s = self.find(get16(self._packet, 2))
            if s is None or len(s.tx) < n:
                return self._fail(Error.BAD_PROTOCOL)
            del s.tx[:n]
            s.sent += n
            if s.console:
                self._relay_turns = 0
            elif self._eligible(self.streams[CONSOLE_SLOT]):
                self._relay_turns += 1
        self.tx_seq += 1
        self._last_tx = now
        self._unpend()
        return True

    # ---- RX -----------------------------------------------------------------------------------

    def receive(self, p: bytes, now: int) -> bool:
        if self.state not in (SState.HELLO, SState.READY):
            return False
        if self.state is SState.HELLO and self._version_signal(p):
            self.rx_seq += 1
            self._last_rx = now
            self.peer_reset = True
            if self._hello_accepted and self.hello_end is HelloEnd.NONE:
                self.hello_end = HelloEnd.VERSION_SIGNAL
            return self._fail(Error.UNSUPPORTED_VERSION)
        size = len(p)
        if (size < HEADER or size > self.value or p[0] != VERSION or p[11] or get16(p, 8) != size - HEADER or
                get32(p, 4) != self.rx_seq or self.rx_seq == U32_MAX):
            return self._fail(Error.BAD_PROTOCOL)
        self.rx_seq += 1
        self._last_rx = now
        op, id_, flags, body = p[1], get16(p, 2), p[10], p[HEADER:]
        n = len(body)
        if op == Op.DATA:
            if flags or self._assembling or not n or self.state is not SState.READY:
                return self._fail(Error.BAD_PROTOCOL)
            s = self.find(id_)
            if s is None:
                return True if self._retired(id_) else self._fail(Error.BAD_PROTOCOL)
            if s.state in (StState.ENDED, StState.CLOSING):
                return True
            if s.state is not StState.OPEN or s.eof:
                return self._fail(Error.BAD_PROTOCOL)
            if n > s.receive_limit - s.received or n > WINDOW - len(s.rx):
                return self._fail(Error.FLOW_VIOLATION)
            s.rx += body
            s.received += n
            return True
        if flags & ~3 or not (flags or self._assembling):
            return self._fail(Error.BAD_PROTOCOL)
        if flags & FIRST:
            if self._assembling:
                return self._fail(Error.BAD_PROTOCOL)
            self._assembling, self._assembly_op, self._assembly_id, self._assembly_at = True, op, id_, now
            self._assembly.clear()
        if not self._assembling or op != self._assembly_op or id_ != self._assembly_id or \
                n > CONTROL - len(self._assembly):
            return self._fail(Error.BAD_PROTOCOL)
        self._assembly += body
        if not flags & LAST:
            return True
        self._assembling = False
        message = bytes(self._assembly)
        self._assembly.clear()
        return self._control(op, id_, message, now)

    # ---- internals ------------------------------------------------------------------------------

    def _retired(self, id_: int) -> bool:
        return bool(id_) and (not self._next_id or id_ < self._next_id)

    def _unpend(self) -> None:
        self._packet = b""
        self._pending = False

    def _cancel_data(self, id_: int) -> None:
        if self._pending and not self._pending_control and get16(self._packet, 2) == id_:
            self._unpend()

    def _teardown(self, why: int) -> None:
        self.state = SState.DOWN
        self.error = why
        self._messages.clear()
        self._assembling = False
        self._pending = False
        self._packet = b""
        self._assembly.clear()
        for s in self.streams:
            if s.live():
                self._end(s, why)

    def _hello_unanswered(self) -> bool:
        return (self.state is SState.HELLO and self._hello_accepted and not self.rx_seq
                and self.hello_end is HelloEnd.NONE)

    def _version_signal(self, p: bytes) -> bool:
        return (len(p) == HEADER + 2 and p[0] in (1, VERSION) and p[1] == Op.RESET and not get16(p, 2) and
                get32(p, 4) == self.rx_seq and get16(p, 8) == 2 and p[10] == (FIRST | LAST) and not p[11] and
                get16(p, HEADER) == Error.UNSUPPORTED_VERSION)

    @staticmethod
    def _eligible(c: Stream) -> bool:
        return c.state is StState.OPEN and not c.deferred() and bool(c.tx) and c.peer_limit > c.sent

    def _next_data(self) -> Stream | None:
        pick = None
        for i in range(RELAY_SLOTS):
            candidate = self.streams[(self._round + i) % RELAY_SLOTS]
            if self._eligible(candidate):
                pick = (self._round + i) % RELAY_SLOTS
                break
        console = self.streams[CONSOLE_SLOT]
        if self._eligible(console) and (pick is None or self._relay_turns >= 3):
            return console
        if pick is None:
            return None
        self._round = (pick + 1) % RELAY_SLOTS
        return self.streams[pick]

    def _link_state(self, now: int) -> bool:
        if self._local_flags == self._sent_flags:
            return True
        body = le32(self._local_flags)
        if (m := self._unstarted(Op.LINK_STATE, 0)) is not None:
            m.body = body
            self._sent_flags = self._local_flags
            return True
        if self._link_state_sent and not expired(now, self._link_state_at, 1000):
            return True
        if not self._queue(Op.LINK_STATE, 0, body):
            return False
        self._sent_flags, self._link_state_at, self._link_state_sent = self._local_flags, now, True
        return True

    def _close_now(self, s: Stream, now: int, reason: int) -> bool:
        if (s.console and not 1 <= reason <= 5) or (not s.console and reason > 1):
            reason = 1
        self._cancel_data(s.id)
        s.tx.clear()
        s.rx.clear()
        s.state, s.close_at, s.draining, s.close_reason = StState.CLOSING, now, False, reason
        self._purge(s.id)
        return self._queue(Op.CLOSE, s.id, le16(reason))

    def _end(self, s: Stream, why: int) -> None:
        self._cancel_data(s.id)
        s.state, s.error, s.draining = StState.ENDED, why, False
        s.tx.clear()
        s.rx.clear()
        self._purge(s.id)

    def _fail(self, why: int) -> bool:
        self.down(why)
        self.state = SState.FAILED
        return False

    def _started_at(self, i: int) -> bool:
        return bool(self._messages[i].offset) or (i == 0 and self._pending and self._pending_control)

    def _unstarted(self, op: Op, id_: int) -> Message | None:
        for i, m in enumerate(self._messages):
            if m.op is op and m.id == id_ and not self._started_at(i):
                return m
        return None

    def _purge(self, id_: int) -> None:
        self._messages = [m for i, m in enumerate(self._messages)
                          if not (m.op is Op.CREDIT and m.id == id_ and not self._started_at(i))]

    def _queue(self, op: Op, id_: int, body: bytes) -> bool:
        if len(body) > CONTROL:
            return self._fail(Error.BAD_PROTOCOL)
        if op is Op.CREDIT or (not id_ and op in (Op.LINK_STATE, Op.PONG)):
            if (m := self._unstarted(op, id_)) is not None:
                m.body = bytes(body)
                return True
        if len(self._messages) == CONTROL_QUEUE:
            return self._fail(Error.NO_RESOURCES)
        self._messages.append(Message(op, id_, bytes(body)))
        return True

    def _control(self, op: int, id_: int, p: bytes, now: int) -> bool:
        n = len(p)
        if not id_:
            if op == Op.RESET and n == 2 and known_error(get16(p)):
                self.peer_reset = True
                return self._fail(get16(p))
            if self.state is SState.HELLO:
                if (self._messages or self._pending or op != Op.HELLO_ACK or n != 20 or p[:8] != self._nonce or
                        p[16] != MAX_STREAMS or p[17] & ~PHONE_FLAG_MASK & 0xFF or get16(p, 18) < 20 or
                        get16(p, 18) > self.value):
                    return self._fail(Error.BAD_PROTOCOL)
                if not any(p[8:16]):
                    return self._fail(Error.BAD_PROTOCOL)
                self.value, self._peer_flags, self.state = get16(p, 18), p[17], SState.READY
                return True
            if op == Op.PING and n == 4:
                return self._queue(Op.PONG, 0, p)
            if op == Op.PONG and n == 4 and self._ping_pending and get32(p) == self._ping_cookie:
                self._ping_pending = False
                self.pongs_received += 1
                return True
            if op == Op.LINK_STATE and n == 4 and not get32(p) & ~PHONE_FLAG_MASK:
                self._peer_flags = p[0]
                return True
            return self._fail(Error.BAD_PROTOCOL)
        if self.state is not SState.READY:
            return self._fail(Error.BAD_PROTOCOL)
        valid = ((op == Op.OPEN_OK and n == 4) or (op == Op.OPEN_ERR and n == 2 and known_error(get16(p))) or
                 (op == Op.CREDIT and n == 4) or (op == Op.FIN and n == 4) or (op == Op.CLOSE and n == 2) or
                 (op == Op.CLOSE_ACK and n == 0) or (op == Op.RESET and n == 2 and known_error(get16(p))))
        if not valid:
            return self._fail(Error.BAD_PROTOCOL)
        s = self.find(id_)
        if op == Op.CLOSE and get16(p) > (1 if s is not None and s.live() and not s.console else 5):
            return self._fail(Error.BAD_PROTOCOL)
        if op == Op.FIN and s is not None and s.console:
            return self._fail(Error.BAD_PROTOCOL)
        if s is None or s.state is StState.ENDED:
            if not self._retired(id_) or op in (Op.OPEN_OK, Op.OPEN_ERR):
                return self._fail(Error.BAD_PROTOCOL)
            return self._queue(Op.CLOSE_ACK, id_, b"") if op == Op.CLOSE else True
        if op == Op.CLOSE:
            self._cancel_data(id_)
            if s.state is not StState.CLOSING:
                s.close_at = now
            s.state = StState.CLOSING
            s.error = Error.CANCELLED if get16(p) else 0
            s.peer_close_reason = get16(p)
            s.draining = False
            s.tx.clear()
            s.rx.clear()
            self._purge(id_)
            return self._queue(Op.CLOSE_ACK, id_, b"")
        if op == Op.RESET:
            self._end(s, get16(p))
            s.remote_end = True
            return True
        if s.state is StState.CLOSING:
            if op == Op.CLOSE_ACK:
                self._end(s, 0)
            elif op == Op.OPEN_ERR:
                self._end(s, get16(p))
                s.remote_end = True
            return True
        if s.state is StState.OPENING:
            if not s.open_submitted:
                return self._fail(Error.BAD_PROTOCOL)
            if op == Op.OPEN_ERR:
                self._end(s, get16(p))
                s.remote_end = True
                return True
            if op != Op.OPEN_OK or not get32(p) or get32(p) > WINDOW:
                return self._fail(Error.BAD_PROTOCOL)
            s.peer_limit, s.state, s.opened_at, s.pressure_at, s.credit_at = get32(p), StState.OPEN, now, now, now
            return True
        if op == Op.CREDIT:
            limit = get32(p)
            if limit < s.peer_limit or limit > s.sent + WINDOW:
                return self._fail(Error.FLOW_VIOLATION)
            s.peer_limit = limit
            return True
        if op == Op.FIN and not s.eof and get32(p) == s.received:
            s.eof = True
            return True
        return self._fail(Error.BAD_PROTOCOL)


# --------------------------------------------------------------------------------------------
# The console driver (reduced BLE_PhonePipeConsole.cpp)
# --------------------------------------------------------------------------------------------

BACKOFF_MS = (2000, 5000, 10000)


class Hold(Enum):
    NONE = "none"
    BACKOFF = "backoff"
    FLAG_CHANGE = "flag_change"
    WANTED_RISE = "wanted_rise"
    STOPPED = "stopped"


class P4Console:
    """The P4's console stream: open/reopen, INFO, COMMAND -> responder -> TEXT, CLOSE 3/4."""

    def __init__(self, responder: Callable[[str], str | None], fw: str = "0.99.98.4-model",
                 model: str = "P4X-EYE") -> None:
        self.responder = responder
        self.fw, self.model = fw, model
        self.out: deque[bytes] = deque()        # whole records waiting for tx space
        # History, kept across sessions.
        self.commands: list[str] = []
        self.binaries: list[bytes] = []
        self.opens = 0
        self.closes: list[int] = []             # CLOSE reasons this P4 sent
        self.ends: list[tuple[int, int, bool]] = []  # (id, error, remote_end) of ended streams
        self.reset_session()

    def reset_session(self) -> None:
        """A new READY session: no stream, no holds, the ladders from the start."""
        self.id = 0
        self.info = False
        self.closing = 0
        self.hold = Hold.NONE
        self.hold_at = self.hold_ms = 0
        self.backoff = self.quiet = 0
        self.wanted_low = False
        self.offered = self.wanted = False
        self.session_seen = False
        self.out.clear()

    def tick(self, pipe: P4Session, now: int) -> None:
        offered = bool(pipe.local_flags & P4_CONSOLE_OFFERED)
        wanted = bool(pipe.peer_flags() & PHONE_CONSOLE_WANTED)
        if not self.session_seen:
            self.session_seen, self.offered, self.wanted = True, offered, wanted
        changed = offered != self.offered or wanted != self.wanted
        self.offered, self.wanted = offered, wanted
        if self.hold is Hold.BACKOFF and expired(now, self.hold_at, self.hold_ms):
            self.hold = Hold.NONE
        elif self.hold is Hold.FLAG_CHANGE and changed:
            self.hold = Hold.NONE
        elif self.hold is Hold.WANTED_RISE:
            if not wanted:
                self.wanted_low = True
            elif self.wanted_low:
                self.hold = Hold.NONE
            elif self.hold_ms and expired(now, self.hold_at, self.hold_ms):
                self.hold = Hold.NONE
                self.quiet += 1
        if self.id:
            self._stream_pass(pipe, now)
        if (not self.id and self.hold is Hold.NONE and offered and wanted and
                pipe.streams[CONSOLE_SLOT].state is StState.FREE):
            if new := pipe.open_console(now):
                self.id = new
                self.opens += 1

    def say(self, rtype: int, data: bytes) -> None:
        """Queue P4 output: TEXT/BLOB as a chain, CARD/MIRROR as standalone records."""
        writer = records.P4Writer()
        writer.egress.reset()
        if rtype in (records.RecordType.TEXT, records.RecordType.BLOB):
            writer.chain(rtype, data)
        else:
            writer.single(rtype, data)
        stream = bytes(writer.stream)
        at = 0
        while at < len(stream):
            size = records.HEADER_SIZE + (stream[at + 2] | stream[at + 3] << 8)
            self.out.append(stream[at:at + size])
            at += size

    def close(self, pipe: P4Session, now: int, reason: int) -> None:
        """A P4-side close (bledisconnect = 2, revocation = 5, ...)."""
        if self.id and not self.closing:
            pipe.close_after_drain(self.id, now, reason)
            self.closing = reason
            self.closes.append(reason)

    def _backoff_hold(self, now: int, stop_after: bool) -> None:
        if self.backoff < 3:
            self.hold, self.hold_at, self.hold_ms = Hold.BACKOFF, now, BACKOFF_MS[self.backoff]
            self.backoff += 1
        else:
            self.hold = Hold.STOPPED if stop_after else Hold.FLAG_CHANGE

    def _wanted_rise_hold(self, now: int) -> None:
        self.hold, self.wanted_low, self.hold_at = Hold.WANTED_RISE, not self.wanted, now
        self.hold_ms = BACKOFF_MS[self.quiet] if self.wanted and self.quiet < 3 else 0

    def _settle(self, s: Stream, now: int) -> None:
        if self.closing == 1:
            self._backoff_hold(now, False)
            return
        if self.closing:
            self.hold = Hold.NONE
            return
        if not s.remote_end:
            self._wanted_rise_hold(now)
        elif s.peer_limit:
            self._backoff_hold(now, False)
        elif s.error == Error.SERVICE_OFF:
            self._wanted_rise_hold(now)
        elif s.error == Error.UNSUPPORTED:
            self.hold = Hold.STOPPED
        else:
            self._backoff_hold(now, s.error == Error.BUSY)

    def _stream_pass(self, pipe: P4Session, now: int) -> None:
        s = pipe.find(self.id)
        if s is None:
            self.id = 0
            return
        if s.state is StState.ENDED:
            self.ends.append((self.id, s.error, s.remote_end))
            self._settle(s, now)
            pipe.release(self.id)
            self.id, self.info, self.closing = 0, False, 0
            self.out.clear()
            return
        is_open = s.state is StState.OPEN
        if s.state in (StState.OPEN, StState.OPENING) and not s.draining and not self.closing:
            why = 3 if not self.offered else 4 if not self.wanted else 0
            if why:
                self.close(pipe, now, why)
        if is_open and not s.draining and not self.closing:
            if not self.info:
                info = records.encode(records.RecordType.INFO, 0, records.build_info(self.fw, self.model))
                pipe.write(self.id, info)
                self.info = True
            else:
                self._ingress(pipe, now)
            while self.out and pipe.tx_space(self.id) >= len(self.out[0]):
                pipe.write(self.id, self.out.popleft())

    def _ingress(self, pipe: P4Session, now: int) -> None:
        while (have := pipe.rx_available(self.id)):
            data = pipe.rx_peek(self.id, have)
            r = records.scan_ingress(data)
            if r.verdict is records.Verdict.MALFORMED:
                self.close(pipe, now, 1)
                return
            if r.verdict is records.Verdict.NEED_MORE:
                return
            payload = data[records.HEADER_SIZE:r.size]
            pipe.consume(self.id, r.size)
            if r.verdict is records.Verdict.SKIP:
                continue
            assert r.header is not None
            if r.header.type == records.RecordType.COMMAND:
                line = payload.decode("utf-8", errors="replace")
                self.commands.append(line)
                self.backoff = self.quiet = 0
                reply = self.responder(line)
                if reply is not None:
                    self.say(records.RecordType.TEXT, reply.encode("utf-8"))
            elif r.header.type == records.RecordType.BINARY:
                self.binaries.append(payload)


# --------------------------------------------------------------------------------------------
# The P4's GATT client (BLE_PhoneGatt.cpp) and owner
# --------------------------------------------------------------------------------------------

class GattPhase(Enum):
    IDLE = "idle"
    SECURE = "secure"
    PIPE = "pipe"
    STOPPED = "stopped"


@dataclass
class P4End:
    """How one P4 link attempt ended."""

    reason: int
    hello_end: HelloEnd
    peer_reset: bool
    phase: GattPhase


def default_responder(line: str) -> str | None:
    """A tiny CLI: what the P4 answers to a few commands."""
    words = line.split()
    if not words:
        return None
    match words[0]:
        case "status":
            return "OK: phone link up"
        case "help":
            return "\n".join(f"line {i}: help text for command {i}" for i in range(60)) + "\nOK"
        case "login":
            return "OK: logged in"
        case "echo":
            return line[5:]
    return f"Error: unknown command {words[0]}"


class P4Gatt:
    """The P4 side of one or more links over a FakePeripheral (see the module docstring).

    ``psk`` is the P4's pipe PSK; ``random`` draws the secure-channel key and nonce and the pipe
    HELLO nonce. ``console_offered`` is CONSOLE_OFFERED (bleconsole on + a device code)."""

    def __init__(self, radio: "FakeRadio", psk: bytes, *, mtu: int = 247, key: str = "c1",
                 console_offered: bool = True, responder: Callable[[str], str | None] = default_responder,
                 random: Callable[[int], bytes] = os.urandom) -> None:
        self.radio = radio
        self.psk = bytes(psk)
        self.mtu = mtu
        self.key = key
        self.console_offered = console_offered
        self.random = random
        self.pipe = P4Session()
        self.console = P4Console(responder)
        self.phase = GattPhase.IDLE
        self.channel: DeviceChannel | None = None
        self.inbox: deque[bytes | None] = deque()   # None: a Service Changed indication
        self.service_changes = 0
        self.reply: bytes | None = None
        self.reject_pending = False
        self.reject_at = self.secure_at = 0
        self.last_send = -1000
        self.ends: list[P4End] = []
        self.values_in: list[bytes] = []
        self.frames_in: list[bytes] = []      # decrypted inner frames from the phone, in order
        self.values_out = 0
        self.ready_sessions = 0
        self._was_ready = False
        # Gatt::poll's pressure inputs, passed to every pipe tick (tests set them).
        self.relay_pressure = False
        self.console_display_busy = False

    # ---- link control -------------------------------------------------------------------------

    def connect(self, now: int) -> bool:
        """Scan, connect, MTU, discovery and the CCCD write: only if the phone advertises."""
        if not self.radio.advertising:
            return False
        self.channel = DeviceChannel(random=self.random)
        self.inbox.clear()
        self.reply, self.reject_pending = None, False
        self.radio.connect(self.key, self.mtu, self.inbox.append, lambda: self.inbox.append(None))
        self.phase, self.secure_at = GattPhase.SECURE, now
        self.radio.subscribe(self.key)
        return True

    def stop(self, now: int, reason: int) -> None:
        if self.phase in (GattPhase.IDLE, GattPhase.STOPPED):
            return
        self.pipe.down(reason, now)
        self.ends.append(P4End(reason, self.pipe.hello_end, self.pipe.peer_reset, self.phase))
        self.phase = GattPhase.STOPPED
        self.channel = None
        self._was_ready = False
        self.radio.disconnect(self.key)

    @property
    def ready(self) -> bool:
        return self.phase is GattPhase.PIPE and self.pipe.state is SState.READY

    # ---- the poll -------------------------------------------------------------------------------

    def poll(self, now: int) -> None:
        while self.inbox and self.phase in (GattPhase.SECURE, GattPhase.PIPE):
            self._consume(self.inbox.popleft(), now)
        if self.phase not in (GattPhase.SECURE, GattPhase.PIPE):
            return
        if self.reject_pending and expired(now, self.reject_at, 250):
            self.stop(now, Error.NOT_AUTHORIZED)
            return
        if self.phase is GattPhase.SECURE and expired(now, self.secure_at, 5000):
            self.stop(now, Error.NOT_AUTHORIZED)
            return
        self.pipe.set_local_flags(P4_CONSOLE_OFFERED if self.console_offered else 0)
        if self.phase is GattPhase.PIPE:
            if self.pipe.state is SState.READY:
                if not self._was_ready:
                    self._was_ready = True
                    self.ready_sessions += 1
                    self.console.reset_session()
                self.console.tick(self.pipe, now)
            if not self.pipe.tick(now, self.relay_pressure, self.console_display_busy):
                self.stop(now, self.pipe.error or Error.BAD_PROTOCOL)
                return
        if not expired(now, self.last_send, 30):
            return
        assert self.channel is not None
        if self.phase is GattPhase.SECURE:
            if self.reply is None:
                return
            self._write(self.reply, now)
            if self.reject_pending:
                self.stop(now, Error.NOT_AUTHORIZED)
                return
            self.reply = None
            if not self.channel.reply_submitted():
                self.stop(now, Error.NOT_AUTHORIZED)
                return
            if self.channel.ready:
                nonce = bytes(8)
                while not any(nonce):
                    nonce = self.random(8)
                if not self.pipe.start(self.mtu, nonce, now, SECURE_OVERHEAD):
                    self.stop(now, Error.BAD_PROTOCOL)
                    return
                self.phase = GattPhase.PIPE
            return
        plain = self.pipe.peek()
        if plain is None:
            return
        value = self.channel.encrypt(plain)
        assert value is not None and len(value) <= value_limit(self.mtu)
        self._write(value, now)
        self.pipe.accept(now)

    def _write(self, value: bytes, now: int) -> None:
        self.last_send = now
        self.values_out += 1
        self.radio.write(self.key, value)

    def _consume(self, value: bytes | None, now: int) -> None:
        if value is None:  # ESP_GATTC_SRVC_CHG_EVT: serviceChanged_=true; stop(now, Protocol)
            self.service_changes += 1
            self.stop(now, Error.BAD_PROTOCOL)
            return
        self.values_in.append(value)
        if self.reject_pending:
            return
        if len(value) > value_limit(self.mtu):
            self.stop(now, Error.BAD_PROTOCOL)
            return
        assert self.channel is not None
        kind, plain = self.channel.receive(value, self.psk)
        if kind == "data":
            self.frames_in.append(plain)
        if kind == "reply" and self.phase is GattPhase.SECURE and self.reply is None:
            self.reply = plain
            return
        if kind == "data" and self.phase is GattPhase.PIPE and self.pipe.receive(plain, now):
            return
        if self.phase is GattPhase.SECURE and kind == "error":
            self.reply, self.reject_pending, self.reject_at = b"\x05\x02", True, now
        elif kind == "data" and self.phase is GattPhase.PIPE:
            self.stop(now, self.pipe.error or Error.BAD_PROTOCOL)
        else:
            self.stop(now, Error.NOT_AUTHORIZED)

    # ---- the P4's own relay users (the LLM HTTP path) ---------------------------------------------

    def open_relay(self, kind: int, host: bytes, port: int, now: int) -> int:
        return self.pipe.open(kind, host, port, now)

    def relay_write(self, stream: int, data: bytes) -> int:
        return self.pipe.write(stream, data)

    def relay_read(self, stream: int) -> bytes:
        return self.pipe.read(stream)

    def relay_close(self, stream: int, now: int) -> bool:
        return self.pipe.close(stream, now)


class FakeRadio:
    """What :class:`P4Gatt` needs from the fake peripheral (implemented by FakePeripheral)."""

    advertising: bool

    def connect(self, key: str, mtu: int, sink: Callable[[bytes], None],
                on_service_changed: Callable[[], None] | None = None) -> None: ...

    def subscribe(self, key: str) -> None: ...

    def write(self, key: str, value: bytes) -> None: ...

    def disconnect(self, key: str) -> None: ...
