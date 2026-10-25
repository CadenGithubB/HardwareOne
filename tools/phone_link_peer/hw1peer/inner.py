"""Phone link inner protocol v2, PHONE side.

One :class:`Session` owns the inner protocol of one authenticated link: the
pipe HELLO/HELLO_ACK, LINK_STATE, three stream slots by class (0-1 relay, 2
console), credit, CLOSE/CLOSE_ACK/RESET/FIN, PING/PONG and the control queue.
It is the phone's counterpart of the firmware's ``BLE_PhonePipeCore.h`` (the P4
side) and follows the Android app's v1 phone side (``PipeSession.kt``,
``PipeEngine.kt``) with the v2 rules of firmware-009 §5.1, §5.3, §5.4 and §6.2.
Where those disagree with the firmware or the X1/X2 fixtures, the firmware and
the fixtures win.

The session is pure: no Bluetooth, crypto, sockets, threads, clocks or logging.
The caller (the GATT layer) drives it under one lock:

* :meth:`Session.receive` takes each decrypted inner frame from the P4;
* :meth:`Session.tick` runs the timers (call it every few milliseconds);
* :meth:`Session.peek` gives the next inner frame to seal and notify, and
  :meth:`Session.accept` consumes it once the stack took the notification
  (the same frame comes back from ``peek`` until then). Call ``peek`` only when
  the :class:`TxGate` is ready: one notification at a time, 30 ms apart;
* local controls: :meth:`Session.set_relay_on`, :meth:`Session.set_console_wanted`,
  ``mismatch``, :meth:`Session.ping`, :meth:`Session.end_session`, the console
  and relay methods;
* when :attr:`Session.done` turns true the GATT layer disconnects, first sending
  :meth:`Session.failure_frame` (sealed) when it returns one.

Frame layout (little endian): ``version u8 (2) | op u8 | stream u16 | seq u32 |
len u16 | flags u8 (bit0 FIRST, bit1 LAST) | 0 u8 | body``. Control messages
are reassembled from FIRST..LAST fragments into at most 272 bytes; DATA frames
carry flags 0.

Phone timing rules (the X1 fixture's ``phone_rules``, which this class
reproduces byte for byte):

* every DATA byte is handed on at once (console) or as soon as the relay reads
  it; credit is ``consumed + 4096``, granted at a send opportunity once that is
  at least 512 B past the last grant or 100 ms after it;
* PING and CLOSE are answered at once; a PING goes out after 5 s without a P4
  frame while an accepted relay stream is open;
* queued control frames go first, in order, then DATA round-robin by stream id
  within the P4's credit.

Nothing here logs; :class:`RelayTarget`'s repr hides the address.
"""

import ipaddress
import os
from collections.abc import Callable
from dataclasses import dataclass, field
from enum import Enum, IntEnum
from typing import Final, Protocol

# ---- Wire constants (BLE_PhonePipeCore.h, PipeWire.kt) ------------------------------------

VERSION: Final = 2
HEADER: Final = 12
VALUE_CAP: Final = 512          # the largest ATT value either side uses
MIN_VALUE: Final = 20           # the smallest inner frame limit either side may offer or choose
MIN_MTU: Final = 64             # the P4 refuses to subscribe below this ATT MTU
SECURE_OVERHEAD: Final = 25     # outer DATA framing: type, counter(8), tag(16)
CONTROL_CAP: Final = 272        # one reassembled control message
WINDOW: Final = 4096            # per-stream ring, and the most credit may run ahead
MAX_STREAMS: Final = 3
RELAY_SLOTS: Final = 2          # slots 0 and 1
CONSOLE_SLOT: Final = 2
CONTROL_QUEUE: Final = 12       # 3 slots x (end, OPEN reply, CREDIT) + PING, PONG, LINK_STATE
FIRST: Final = 0x01
LAST: Final = 0x02
WHOLE: Final = FIRST | LAST
U32_MAX: Final = 0xFFFF_FFFF

RELAY_LIMIT: Final = 1024       # OPEN_OK for a relay stream (the app's initial window)
CONSOLE_LIMIT: Final = 4096     # OPEN_OK for the console (firmware-009 §6.2 #4)
CONSOLE_PORT: Final = 1
CREDIT_BYTES: Final = 512       # credit coalescing (both sides)
CREDIT_MS: Final = 100

HELLO_DEADLINE_MS: Final = 3_000        # from the session's start (CONFIRM_ACK verified)
REASSEMBLY_DEADLINE_MS: Final = 2_000
CONNECT_DEADLINE_MS: Final = 2_500      # one relay TCP connect, from the OPEN
PING_IDLE_MS: Final = 5_000             # relay PING after this much RX silence
PONG_DEADLINE_MS: Final = 5_000
SILENCE_MS: Final = 150_000             # no P4 frame for this long: the session fails
CLOSE_ACK_DEADLINE_MS: Final = 2_000    # our CLOSE went out and no CLOSE_ACK came
LINK_STATE_SPACING_MS: Final = 1_000    # LINK_STATE at most once per second (queue time)
NOTIFY_GAP_MS: Final = 30               # at least this between two notifications

# Flag bytes. Any other bit set is a protocol error.
CONSOLE_OFFERED: Final = 0x01           # the P4's, in HELLO and its LINK_STATE
P4_FLAG_MASK: Final = CONSOLE_OFFERED
RELAY_ON: Final = 0x01                  # the phone's, in HELLO_ACK and its LINK_STATE
CONSOLE_WANTED: Final = 0x02
PHONE_FLAG_MASK: Final = RELAY_ON | CONSOLE_WANTED

#: The version signal (firmware-009 §5.4, §6.2 #8): a stream-0 RESET 2 under a v1 header,
#: sent as the phone's first inner frame (seq 0) instead of HELLO_ACK.
VERSION_RESET: Final = bytes.fromhex("01250000" "00000000" "0200" "03" "00" "0200")


class Op(IntEnum):
    HELLO = 0x01
    HELLO_ACK = 0x02
    OPEN = 0x10
    OPEN_OK = 0x11
    OPEN_ERR = 0x12
    DATA = 0x20
    CREDIT = 0x21
    FIN = 0x22
    CLOSE = 0x23
    CLOSE_ACK = 0x24
    RESET = 0x25
    PING = 0x30
    PONG = 0x31
    LINK_STATE = 0x32


_OPS: Final = frozenset(int(op) for op in Op)


class Error(IntEnum):
    """Fixed numeric errors (OPEN_ERR, stream RESET and session RESET alike)."""

    BAD_PROTOCOL = 1
    UNSUPPORTED_VERSION = 2
    NOT_AUTHORIZED = 3
    TARGET_DENIED = 4
    VPN_UNAVAILABLE = 5
    DNS_FAILED = 6
    CONNECT_FAILED = 7
    CONNECT_TIMEOUT = 8
    BUSY = 9
    NO_RESOURCES = 10
    TCP_IO = 11
    FLOW_VIOLATION = 12
    DEADLINE = 13
    CANCELLED = 14
    DISPLAY_PRIORITY = 15
    SERVICE_OFF = 16
    UNSUPPORTED = 17
    PAUSED = 18


class ConsoleClose(IntEnum):
    """The console CLOSE reasons the P4 sends (it accepts 0..5 from the phone)."""

    NORMAL = 0
    ERROR = 1
    RENEWED = 2
    CONSOLE_OFF = 3
    NOT_WANTED = 4
    REVOKED = 5


#: What the app shows for each P4 console CLOSE (spec §2.2); empty means nothing to show.
CONSOLE_CLOSE_TEXT: Final = {
    ConsoleClose.NORMAL: "Console closed",
    ConsoleClose.ERROR: "Console error; reconnecting",
    ConsoleClose.RENEWED: "Session renewed",
    ConsoleClose.CONSOLE_OFF: "Console turned off on the device",
    ConsoleClose.NOT_WANTED: "",
    ConsoleClose.REVOKED: "Signed out by the device",
}


class AddressType(IntEnum):
    IPV4 = 1
    IPV6 = 2
    DNS = 3
    LOCAL = 4  # the console: valid on the wire only, never a relay destination


def known_error(code: int) -> bool:
    """Codes 1..18 are valid in OPEN_ERR and both kinds of RESET; anything else is BAD_PROTOCOL."""
    return 1 <= code <= Error.PAUSED


# ---- Sizes --------------------------------------------------------------------------------

def value_limit(mtu: int) -> int:
    """``V = min(ATT_MTU - 3, 512)``; 0 below the ATT minimum (23)."""
    return 0 if mtu < 23 else min(mtu - 3, VALUE_CAP)


def frame_limit(mtu: int) -> int:
    """``P = V - 25``: the largest inner frame one secure DATA value carries; 0 below MIN_MTU."""
    return 0 if mtu < MIN_MTU else value_limit(mtu) - SECURE_OVERHEAD


def frame_limit_for_update_length(max_update: int) -> int:
    """P from CoreBluetooth's ``maximumUpdateValueLength`` (ATT_MTU - 3), as :func:`frame_limit`."""
    return frame_limit(max_update + 3)


# ---- Frames -------------------------------------------------------------------------------

def _get16(b: bytes | bytearray, at: int) -> int:
    return b[at] | b[at + 1] << 8


def _get32(b: bytes | bytearray, at: int) -> int:
    return _get16(b, at) | _get16(b, at + 2) << 16


def le16(value: int) -> bytes:
    return value.to_bytes(2, "little")


def le32(value: int) -> bytes:
    return value.to_bytes(4, "little")


def build_frame(op: int, stream: int, seq: int, body: bytes = b"", flags: int = WHOLE,
                version: int = VERSION) -> bytes:
    """One inner frame. No checks beyond field widths: tests use it to build bad frames too."""
    return (bytes((version, op)) + le16(stream) + le32(seq) + le16(len(body)) + bytes((flags, 0))
            + bytes(body))


@dataclass(frozen=True, slots=True)
class Frame:
    """A parsed inner frame header plus its body (structure only, no protocol checks)."""

    version: int
    op: int
    stream: int
    seq: int
    flags: int
    body: bytes

    @classmethod
    def parse(cls, frame: bytes | bytearray) -> Frame | None:
        """None unless the frame has a 12-byte header whose length field matches."""
        if len(frame) < HEADER or _get16(frame, 8) != len(frame) - HEADER:
            return None
        return cls(frame[0], frame[1], _get16(frame, 2), _get32(frame, 4), frame[10], bytes(frame[HEADER:]))


# ---- Relay destinations -------------------------------------------------------------------

def _dns_byte(c: int) -> bool:
    return 0x61 <= c <= 0x7A or 0x30 <= c <= 0x39 or c in (0x2D, 0x2E)  # a-z 0-9 - .


@dataclass(frozen=True, slots=True)
class RelayTarget:
    """A relay OPEN's destination exactly as the P4 encodes it. The repr hides host and port."""

    kind: AddressType
    host: bytes = field(repr=False)
    port: int = field(repr=False)

    @classmethod
    def from_wire(cls, kind: int, host: bytes, port: int) -> RelayTarget | None:
        """IPv4 = 4 bytes, IPv6 = 16, DNS = 1-253 bytes of ``[a-z0-9.-]`` (PipeTarget.fromWire);
        None for anything else, which the session treats as BAD_PROTOCOL."""
        if not 1 <= port <= 0xFFFF:
            return None
        match kind:
            case AddressType.IPV4:
                ok = len(host) == 4
            case AddressType.IPV6:
                ok = len(host) == 16
            case AddressType.DNS:
                ok = 1 <= len(host) <= 253 and all(_dns_byte(c) for c in host)
            case _:
                ok = False
        return cls(AddressType(kind), bytes(host), port) if ok else None

    def host_text(self) -> str:
        """The host for a socket call (never log it)."""
        if self.kind is AddressType.DNS:
            return self.host.decode("ascii")
        return str(ipaddress.ip_address(self.host))


# ---- What the session reports ------------------------------------------------------------

class StreamKind(Enum):
    RELAY = "relay"
    CONSOLE = "console"


class StreamState(Enum):
    CONNECTING = "connecting"  # a relay OPEN waiting for its TCP connect
    OPEN = "open"              # OPEN_OK queued or sent
    CLOSING = "closing"        # our CLOSE queued or sent; waiting for CLOSE_ACK


class EndHow(Enum):
    """How a stream left the session (``code`` in :class:`StreamEnd` says the rest)."""

    PEER_CLOSE = "peer_close"    # the P4's CLOSE; code = its reason (answered with CLOSE_ACK)
    PEER_RESET = "peer_reset"    # the P4's stream RESET; code = its error
    LOCAL_CLOSE = "local_close"  # our CLOSE, acknowledged; code = our reason
    LOCAL_RESET = "local_reset"  # our stream RESET; code = our error
    REFUSED = "refused"          # our OPEN_ERR (a relay that never opened); code = our error
    SESSION = "session"          # the session ended under it; code = the session error (0: none)


@dataclass(frozen=True, slots=True)
class StreamEnd:
    stream: int
    how: EndHow
    code: int


class EndKind(Enum):
    """Why the session is over."""

    FAILED = "failed"            # we refused something from the P4: send failure_frame(), disconnect
    PEER_RESET = "peer_reset"    # the P4 sent a stream-0 RESET
    LOCAL_RESET = "local_reset"  # we sent a stream-0 RESET (end_session, e.g. PAUSED)
    VERSION = "version"          # we sent the version signal (a mismatch strike)
    LINK_LOST = "link_lost"      # the GATT layer reported the link gone


@dataclass(frozen=True, slots=True)
class SessionEnd:
    kind: EndKind
    error: int  # an Error code; 0 for a link loss

    @property
    def strike(self) -> bool:
        """This end counts toward the phone's version-mismatch latch."""
        return self.kind is EndKind.VERSION


class State(Enum):
    AWAIT_HELLO = "await_hello"
    READY = "ready"      # HELLO_ACK queued; streams may open
    ENDING = "ending"    # our session RESET (PAUSED or the version signal) is queued; nothing else
    ENDED = "ended"      # see Session.end


@dataclass(frozen=True, slots=True)
class StreamInfo:
    """A stream snapshot for status output (no destination)."""

    stream: int
    kind: StreamKind
    state: StreamState
    sent: int
    received: int
    peer_limit: int
    receive_limit: int
    up_buffered: int
    down_buffered: int


@dataclass(slots=True)
class Stats:
    """Counters for ``status``/``stats``: codes and counts only."""

    frames_in: int = 0
    frames_out: int = 0
    opens: int = 0
    refused: dict[int, int] = field(default_factory=dict)       # OPEN_ERR by code
    local_resets: dict[int, int] = field(default_factory=dict)  # our stream RESETs by code
    peer_closes: int = 0
    peer_resets: int = 0
    link_states_in: int = 0
    link_states_out: int = 0
    pings_in: int = 0
    pings_out: int = 0
    console_bytes_in: int = 0
    console_bytes_out: int = 0
    relay_bytes_in: int = 0
    relay_bytes_out: int = 0


# ---- Endpoints ------------------------------------------------------------------------------

class ConsoleSink(Protocol):
    """Where the console stream goes (records.ConsoleEndpoint behind an adapter, in the app).

    Calls arrive inline from :meth:`Session.receive` and may call back into the session (for
    example :meth:`Session.console_send`). The sink never blocks: the P4 paces the console by
    withholding CREDIT (firmware-009 §6.2 #5).
    """

    def on_console_open(self, stream: int) -> None:
        """A console stream was admitted (OPEN_OK 4096 queued). The record stream starts fresh."""

    def on_console_bytes(self, data: bytes) -> bool | None:
        """P4 record bytes, in order, any split. Return False when they hold a malformed record:
        the session answers with stream RESET 1 (§6.2 #6). None counts as True."""

    def on_console_end(self, end: StreamEnd) -> None:
        """The console stream is gone."""


#: RelayEndpoint.relay_open answers: ACCEPT gives OPEN_OK at once; CONNECTING (None) means the
#: endpoint will call Session.relay_connected or Session.relay_fail later; 1..18 refuses.
ACCEPT: Final = 0
CONNECTING: Final = None


class RelayEndpoint(Protocol):
    """The relay side (relay.py's TCP relay, or a test fake). Calls arrive inline."""

    def relay_open(self, stream: int, target: RelayTarget, now: int) -> int | None:
        """An admitted relay OPEN (RELAY_ON is set and a slot is free): ACCEPT, CONNECTING or an
        error code (the app's: 4 target denied, 5 VPN, 10 resources, 16 no destination, ...)."""

    def relay_data(self, stream: int) -> None:
        """P4 bytes are waiting (Session.relay_read), or the P4's FIN arrived
        (Session.relay_shutdown_due)."""

    def relay_end(self, stream: int, end: StreamEnd) -> None:
        """The stream is gone: close its socket."""


# ---- The TX gate ----------------------------------------------------------------------------

@dataclass(slots=True)
class TxGate:
    """One notification in flight and at least ``min_gap_ms`` between submissions.

    The GATT layer calls :meth:`submitted` when it hands a value to the stack and
    :meth:`completed` when the stack has taken it (at once when CoreBluetooth's
    ``updateValue`` returns YES; on ``peripheralManagerIsReadyToUpdateSubscribers``
    after a NO and a successful retry).
    """

    min_gap_ms: int = NOTIFY_GAP_MS
    in_flight: bool = False
    last_at: int | None = None

    def ready(self, now: int) -> bool:
        return not self.in_flight and (self.last_at is None or now - self.last_at >= self.min_gap_ms)

    def submitted(self, now: int) -> None:
        self.in_flight = True
        self.last_at = now

    def completed(self) -> None:
        self.in_flight = False

    def reset(self) -> None:
        self.in_flight = False
        self.last_at = None


# ---- Internal state -------------------------------------------------------------------------

@dataclass(slots=True, eq=False)
class _Stream:
    stream: int
    kind: StreamKind
    state: StreamState
    opened_at: int
    target: RelayTarget | None = None
    peer_limit: int = 0       # the P4's allowance for our DATA (its OPEN limit, then CREDIT)
    sent: int = 0             # DATA body bytes accepted by the stack
    received: int = 0         # DATA body bytes from the P4
    consumed: int = 0         # ... handed on (console) or read by the relay
    receive_limit: int = 0    # our grant the P4 has been sent (OPEN_OK, then CREDIT)
    granted: int = 0          # our latest grant, queued or sent
    grant_at: int = 0
    ok_sent: bool = False     # OPEN_OK accepted by the stack
    up: bytearray = field(default_factory=bytearray)    # to the P4
    down: bytearray = field(default_factory=bytearray)  # from the P4, waiting for relay_read
    peer_fin: bool = False
    local_eof: bool = False
    fin_queued: bool = False
    output_shut: bool = False
    close_reason: int = 0
    close_sent_at: int | None = None


@dataclass(slots=True, eq=False)
class _Message:
    op: Op
    stream: int
    body: bytes
    offset: int = 0


@dataclass(slots=True)
class _Pending:
    frame: bytes
    control: bool
    stream: int
    size: int       # body bytes
    raw: bool = False


def _wipe(buffer: bytearray) -> None:
    """Best effort: zero a buffer before dropping it (Python keeps no other copy of ours)."""
    buffer[:] = bytes(len(buffer))
    buffer.clear()


# ---- The session ----------------------------------------------------------------------------

class Session:
    """The phone's half of inner protocol v2 for ONE authenticated link.

    ``max_frame`` is P for this link (:func:`frame_limit`); ``now`` is the session's start in
    milliseconds (CONFIRM_ACK verified), which starts the 3 s HELLO deadline. ``random`` draws
    the HELLO_ACK nonce and PING cookies. A new link needs a new Session: nothing survives a
    reconnect.
    """

    def __init__(self, max_frame: int, now: int, *, console: ConsoleSink | None = None,
                 relay: RelayEndpoint | None = None, relay_on: bool = False, console_wanted: bool = False,
                 mismatch: bool = False, random: Callable[[int], bytes] = os.urandom) -> None:
        if not MIN_VALUE <= max_frame <= VALUE_CAP:
            raise ValueError(f"max_frame must be {MIN_VALUE}..{VALUE_CAP}")
        self._console = console
        self._relay = relay
        self._relay_on = relay_on
        self._console_wanted = console_wanted
        #: Answer the next pipe HELLO with the version signal even when it is v2 (a strike test).
        self.mismatch = mismatch
        self._random = random
        self.stats = Stats()

        self._state = State.AWAIT_HELLO
        self._end: SessionEnd | None = None
        self._value = max_frame
        self._started = now
        self._last_rx = now
        self._tx_seq = 0
        self._rx_seq = 0
        self._hello_ack_sent = False
        self._device_nonce: bytes | None = None
        self._p4_flags = 0

        self._slots: list[_Stream | None] = [None] * MAX_STREAMS
        self._highest = 0
        self._rr = 0  # the id of the stream that sent DATA last

        self._queue: list[_Message] = []
        self._pending: _Pending | None = None
        self._raw: bytes | None = None  # the version signal, sent instead of anything queued

        self._assembling = False
        self._assembly = bytearray()
        self._assembly_op = 0
        self._assembly_stream = 0
        self._assembly_at = 0

        self._ping_pending = False
        self._ping_cookie = b""
        self._ping_at = 0

        self._sent_flags = 0
        self._link_sent = False
        self._link_at = 0
        self._link_forced = False

    # ---- status ---------------------------------------------------------------------------

    @property
    def state(self) -> State:
        return self._state

    @property
    def end(self) -> SessionEnd | None:
        """Why the session ended (set once the session is ENDING or ENDED)."""
        return self._end

    @property
    def done(self) -> bool:
        """The session is over: the GATT layer sends failure_frame() if any, then disconnects."""
        return self._state is State.ENDED

    @property
    def ready(self) -> bool:
        return self._state is State.READY

    @property
    def hello_ack_sent(self) -> bool:
        """HELLO_ACK has been accepted by the stack (the phone's local readiness)."""
        return self._hello_ack_sent

    @property
    def value(self) -> int:
        """The inner frame limit: P until the pipe HELLO, then the negotiated value."""
        return self._value

    @property
    def device_nonce(self) -> bytes | None:
        """The P4's pipe HELLO nonce (the session's correlation tag in both sides' logs)."""
        return self._device_nonce

    @property
    def p4_flags(self) -> int:
        """The P4's flags from HELLO and its latest LINK_STATE (0 unless READY)."""
        return self._p4_flags if self._state is State.READY else 0

    @property
    def console_offered(self) -> bool:
        return bool(self.p4_flags & CONSOLE_OFFERED)

    @property
    def local_flags(self) -> int:
        """Our flag byte as HELLO_ACK and LINK_STATE carry it."""
        flags = 0
        if self._relay_on and self._relay is not None:
            flags |= RELAY_ON
        if self._console_wanted and self._console is not None:
            flags |= CONSOLE_WANTED
        return flags

    @property
    def tx_seq(self) -> int:
        return self._tx_seq

    @property
    def rx_seq(self) -> int:
        return self._rx_seq

    @property
    def queued_control(self) -> int:
        return len(self._queue)

    @property
    def ping_pending(self) -> bool:
        return self._ping_pending

    def streams(self) -> list[StreamInfo]:
        return [StreamInfo(s.stream, s.kind, s.state, s.sent, s.received, s.peer_limit, s.receive_limit,
                           len(s.up), len(s.down)) for s in self._live()]

    def console_stream(self) -> int:
        """The console stream id, 0 when none is open."""
        s = self._slots[CONSOLE_SLOT]
        return s.stream if s is not None else 0

    # ---- local controls -------------------------------------------------------------------

    def set_relay_on(self, on: bool) -> None:
        """RELAY_ON (switch on, destination saved, network usable). A change after READY goes
        out as LINK_STATE; while it is off every relay OPEN is refused with 16."""
        self._relay_on = on

    def set_console_wanted(self, wanted: bool) -> None:
        """CONSOLE_WANTED. While it is 0 a console OPEN is refused with 16; dropping it while the
        console is open asks the P4 to close it (CLOSE 4)."""
        self._console_wanted = wanted

    def ping(self, now: int) -> bool:
        """Queue a PING now (one outstanding at a time); its PONG is due within 5 s."""
        if self._state is not State.READY or self._ping_pending:
            return False
        return self._queue_ping(now)

    def end_session(self, code: int = Error.PAUSED) -> bool:
        """End the session with a stream-0 RESET (18 PAUSED by default): queued behind what is
        already queued; once it is accepted the session is ENDED."""
        if not known_error(code):
            raise ValueError(f"unknown error code {code}")
        if self._state is not State.READY:
            return False
        if not self._queue_message(Op.RESET, 0, le16(code)):
            return False
        self._state = State.ENDING
        self._end = SessionEnd(EndKind.LOCAL_RESET, code)
        return True

    def link_lost(self) -> None:
        """The GATT layer lost the link: every stream ends, nothing more is sent."""
        if self._state is not State.ENDED:
            self._finish(SessionEnd(EndKind.LINK_LOST, 0))

    # ---- console --------------------------------------------------------------------------

    def console_space(self) -> int:
        s = self._slots[CONSOLE_SLOT]
        return WINDOW - len(s.up) if self._state is State.READY and s and s.state is StreamState.OPEN else 0

    def console_send(self, data: bytes) -> bool:
        """Queue whole console records (COMMAND/BINARY) for the P4: all of ``data`` or nothing."""
        s = self._slots[CONSOLE_SLOT]
        if (self._state is not State.READY or s is None or s.state is not StreamState.OPEN or not data
                or len(data) > WINDOW - len(s.up)):
            return False
        s.up += data
        self.stats.console_bytes_out += len(data)
        return True

    def console_close(self, reason: int = ConsoleClose.NORMAL) -> bool:
        """Close the console with CLOSE ``reason`` (0..5). Unsent console bytes are dropped. A
        LINK_STATE with our current flags follows (§6.2 #3)."""
        if not 0 <= reason <= ConsoleClose.REVOKED:
            raise ValueError("console CLOSE reasons are 0..5")
        s = self._slots[CONSOLE_SLOT]
        if self._state is not State.READY or s is None or s.state is not StreamState.OPEN:
            return False
        self._purge(s.stream, (Op.CREDIT,))
        _wipe(s.up)
        s.state = StreamState.CLOSING
        s.close_reason = reason
        self._link_forced = True
        return self._queue_message(Op.CLOSE, s.stream, le16(reason))

    def console_reset(self, code: int = Error.BAD_PROTOCOL) -> bool:
        """End the console with a stream RESET (1 for a malformed P4 record, §6.2 #6)."""
        if not known_error(code):
            raise ValueError(f"unknown error code {code}")
        s = self._slots[CONSOLE_SLOT]
        if s is None or self._state is not State.READY or s.state is not StreamState.OPEN:
            return False
        return self._local_end(s, code)

    # ---- relay ----------------------------------------------------------------------------

    def relay_connected(self, stream: int, now: int) -> bool:
        """The relay's single TCP connect succeeded: OPEN_OK 1024."""
        s = self._relay_stream(stream)
        if self._state is not State.READY or s is None or s.state is not StreamState.CONNECTING:
            return False
        s.state = StreamState.OPEN
        s.granted, s.grant_at = RELAY_LIMIT, now
        return self._queue_message(Op.OPEN_OK, stream, le32(RELAY_LIMIT))

    def relay_fail(self, stream: int, code: int) -> bool:
        """The relay failed (connect, TCP, VPN, destination change): OPEN_ERR until OPEN_OK has
        started going out, RESET after."""
        if not known_error(code):
            raise ValueError(f"unknown error code {code}")
        s = self._relay_stream(stream)
        if s is None or self._state is not State.READY or s.state is StreamState.CLOSING:
            return False
        return self._local_end(s, code)

    def relay_space(self, stream: int) -> int:
        s = self._relay_stream(stream)
        if self._state is not State.READY or s is None or s.state is not StreamState.OPEN or s.local_eof:
            return 0
        return WINDOW - len(s.up)

    def relay_send(self, stream: int, data: bytes) -> int:
        """TCP bytes for the P4; returns how many fit (never more than relay_space)."""
        n = min(len(data), self.relay_space(stream))
        if n:
            s = self._relay_stream(stream)
            assert s is not None
            s.up += data[:n]
            self.stats.relay_bytes_out += n
        return n

    def relay_eof(self, stream: int) -> None:
        """TCP EOF: FIN goes once every byte read before it has been sent."""
        s = self._relay_stream(stream)
        if s is not None and s.state is StreamState.OPEN:
            s.local_eof = True

    def relay_read(self, stream: int, limit: int = WINDOW) -> bytes:
        """Take up to ``limit`` P4 bytes for the socket; they count as consumed (credit)."""
        s = self._relay_stream(stream)
        if self._state is not State.READY or s is None or s.state is not StreamState.OPEN or not s.down:
            return b""
        data = bytes(s.down[:limit])
        s.down[:len(data)] = bytes(len(data))
        del s.down[:len(data)]
        s.consumed += len(data)
        return data

    def relay_shutdown_due(self, stream: int) -> bool:
        """The P4's FIN has drained to the socket: time for shutdown(SHUT_WR)."""
        s = self._relay_stream(stream)
        return (self._state is State.READY and s is not None and s.state is StreamState.OPEN and s.peer_fin
                and not s.down and not s.output_shut)

    def relay_output_shut(self, stream: int) -> None:
        s = self._relay_stream(stream)
        if s is not None:
            s.output_shut = True

    # ---- P4 -> phone ----------------------------------------------------------------------

    def receive(self, frame: bytes, now: int) -> bool:
        """Take one decrypted inner frame. False when the session is (now) over; a protocol
        violation fails it (``end.kind`` FAILED) and the GATT layer disconnects."""
        if self._state is State.ENDED:
            return False
        if self._state is State.ENDING:
            return True  # our session RESET is on its way: nothing from the P4 matters now
        self.stats.frames_in += 1
        frame = bytes(frame)
        size = len(frame)
        if size < HEADER or size > self._value or _get16(frame, 8) != size - HEADER:
            return self._fail(Error.BAD_PROTOCOL)
        if self._state is State.AWAIT_HELLO and frame[0] != VERSION:
            return self._send_version_signal()  # §6.2 #8: any inner version other than 2
        if frame[0] != VERSION or frame[11] or _get32(frame, 4) != self._rx_seq or self._rx_seq == U32_MAX:
            return self._fail(Error.BAD_PROTOCOL)
        self._rx_seq += 1
        self._last_rx = now
        op, stream, flags, body = frame[1], _get16(frame, 2), frame[10], frame[HEADER:]
        if op not in _OPS:
            return self._fail(Error.BAD_PROTOCOL)
        if op == Op.DATA:
            return self._data(stream, flags, body)
        return self._fragment(Op(op), stream, flags, body, now)

    def _data(self, stream: int, flags: int, body: bytes) -> bool:
        if flags or self._assembling or not body or not stream or self._state is not State.READY:
            return self._fail(Error.BAD_PROTOCOL)
        s = self._find(stream)
        if s is None:
            return True if self._retired(stream) else self._fail(Error.BAD_PROTOCOL)
        if s.state is StreamState.CLOSING:
            return True  # crossed our CLOSE: discarded
        if s.state is not StreamState.OPEN or not s.ok_sent or s.peer_fin:
            return self._fail(Error.BAD_PROTOCOL)
        if s.received + len(body) > s.receive_limit or len(body) > WINDOW - len(s.down):
            return self._fail(Error.FLOW_VIOLATION)
        s.received += len(body)
        if s.kind is StreamKind.CONSOLE:
            return self._console_bytes(s, body)
        s.down += body
        self.stats.relay_bytes_in += len(body)
        if self._relay is not None:
            self._relay.relay_data(stream)
        return self._state is not State.ENDED

    def _console_bytes(self, s: _Stream, body: bytes) -> bool:
        s.consumed += len(body)  # handed on at once: the sink never blocks
        self.stats.console_bytes_in += len(body)
        ok = self._console.on_console_bytes(body) if self._console is not None else True
        if ok is False and self._slots[CONSOLE_SLOT] is s and s.state is StreamState.OPEN:
            self._local_end(s, Error.BAD_PROTOCOL)  # a malformed record: stream RESET 1
        return self._state is not State.ENDED

    def _fragment(self, op: Op, stream: int, flags: int, body: bytes, now: int) -> bool:
        """Control: FIRST [continuation...] LAST, contiguous, one op and stream, <= 272 bytes."""
        # `not body and flags != WHOLE` is stricter than the P4 core, which accepts an empty
        # fragment (BLE_PhonePipeCore.h receive). It is the app's rule, not a contract rule: the
        # P4's peek only produces an empty control frame as FIRST|LAST (offset == len == 0).
        if flags & ~WHOLE or (not flags and not self._assembling) or (not body and flags != WHOLE):
            return self._fail(Error.BAD_PROTOCOL)
        if flags & FIRST:
            if self._assembling:
                return self._fail(Error.BAD_PROTOCOL)
            self._assembling, self._assembly_op, self._assembly_stream, self._assembly_at = True, op, stream, now
            self._assembly.clear()
        if (not self._assembling or op != self._assembly_op or stream != self._assembly_stream
                or len(body) > CONTROL_CAP - len(self._assembly)):
            return self._fail(Error.BAD_PROTOCOL)
        self._assembly += body
        if not flags & LAST:
            return True
        self._assembling = False
        message = bytes(self._assembly)
        _wipe(self._assembly)
        return self._control(op, stream, message, now)

    def _control(self, op: Op, stream: int, b: bytes, now: int) -> bool:
        if stream == 0:
            return self._link_control(op, b)
        if self._state is not State.READY:
            return self._fail(Error.BAD_PROTOCOL)
        n = len(b)
        valid = ((op is Op.OPEN and n >= 8 and n == 8 + b[7]) or (op in (Op.CREDIT, Op.FIN) and n == 4)
                 or (op is Op.CLOSE and n == 2) or (op is Op.CLOSE_ACK and n == 0)
                 or (op is Op.RESET and n == 2 and known_error(_get16(b, 0))))
        if not valid:
            return self._fail(Error.BAD_PROTOCOL)
        if op is Op.OPEN:
            return self._open(stream, b, now)
        s = self._find(stream)
        if s is None:
            # Refused, reset, ended or retired: CLOSE 0..5 gets CLOSE_ACK (§6.2 #2), other late
            # frames are dropped. A frame for an id never opened is a session error.
            if stream > self._highest:
                return self._fail(Error.BAD_PROTOCOL)
            if op is Op.CLOSE:
                if _get16(b, 0) > ConsoleClose.REVOKED:
                    return self._fail(Error.BAD_PROTOCOL)
                return self._queue_message(Op.CLOSE_ACK, stream, b"")
            return True
        return self._stream_control(s, op, b)

    def _link_control(self, op: Op, b: bytes) -> bool:
        n = len(b)
        if op is Op.RESET and n == 2 and known_error(_get16(b, 0)):
            self.stats.peer_resets += 1
            return self._finish(SessionEnd(EndKind.PEER_RESET, _get16(b, 0)))
        if self._state is State.AWAIT_HELLO:
            return self._hello(b) if op is Op.HELLO else self._fail(Error.BAD_PROTOCOL)
        if op is Op.PING and n == 4:
            self.stats.pings_in += 1
            return self._queue_message(Op.PONG, 0, b)
        if op is Op.PONG and n == 4 and self._ping_pending and b == self._ping_cookie:
            self._ping_pending = False
            return True
        if op is Op.LINK_STATE and n == 4 and not _get32(b, 0) & ~P4_FLAG_MASK:
            self.stats.link_states_in += 1
            self._p4_flags = b[0]
            return True
        return self._fail(Error.BAD_PROTOCOL)

    def _stream_control(self, s: _Stream, op: Op, b: bytes) -> bool:
        console = s.kind is StreamKind.CONSOLE
        if op is Op.CLOSE:
            reason = _get16(b, 0)
            if reason > (ConsoleClose.REVOKED if console else 1):  # relay 0..1, console 0..5
                return self._fail(Error.BAD_PROTOCOL)
            self.stats.peer_closes += 1
            self._purge(s.stream)
            self._end_stream(s, EndHow.PEER_CLOSE, reason)
            return self._queue_message(Op.CLOSE_ACK, s.stream, b"")
        if op is Op.RESET:
            self._purge(s.stream)
            self._end_stream(s, EndHow.PEER_RESET, _get16(b, 0))
            return True
        if op is Op.CLOSE_ACK:
            if s.state is StreamState.CLOSING and s.close_sent_at is not None:
                self._end_stream(s, EndHow.LOCAL_CLOSE, s.close_reason)
                return True
            return self._fail(Error.BAD_PROTOCOL)  # we never sent a CLOSE on it
        if op is Op.FIN and console:
            return self._fail(Error.BAD_PROTOCOL)  # the console has no TCP end (§5.1)
        if s.state is StreamState.CLOSING:
            return True  # CREDIT or FIN that crossed our CLOSE
        if s.state is not StreamState.OPEN or not s.ok_sent:
            return self._fail(Error.BAD_PROTOCOL)
        if op is Op.CREDIT:
            limit = _get32(b, 0)
            if limit < s.peer_limit or limit > s.sent + WINDOW:
                return self._fail(Error.FLOW_VIOLATION)
            s.peer_limit = limit
            return True
        # FIN on a relay stream: once, at exactly the bytes we received.
        if s.peer_fin or _get32(b, 0) != s.received:
            return self._fail(Error.BAD_PROTOCOL)
        s.peer_fin = True
        if self._relay is not None:
            self._relay.relay_data(s.stream)
        return self._state is not State.ENDED

    # ---- HELLO and OPEN admission ---------------------------------------------------------

    def _hello(self, b: bytes) -> bool:
        if (len(b) != 12 or not any(b[:8]) or b[8] != MAX_STREAMS or b[9] & ~P4_FLAG_MASK):
            return self._fail(Error.BAD_PROTOCOL)
        offer = _get16(b, 10)
        # The upper bound is stricter than the contract requires: the P4 offers
        # valueLimit(mtu) - 25, at most 487 (BLE_PhonePipeCore.h start), so 512 is never reached.
        if not MIN_VALUE <= offer <= VALUE_CAP:
            return self._fail(Error.BAD_PROTOCOL)
        self._device_nonce = b[:8]
        self._p4_flags = b[9]
        if self.mismatch:
            return self._send_version_signal()
        chosen = min(self._value, offer)
        nonce = bytes(8)
        for _ in range(8):
            if any(nonce):
                break
            nonce = self._random(8)
            if len(nonce) != 8:
                raise RuntimeError("random source returned the wrong size")
        if not any(nonce):
            return self._fail(Error.NO_RESOURCES)
        flags = self.local_flags
        ack = b[:8] + nonce + bytes((MAX_STREAMS, flags)) + le16(chosen)
        self._value = chosen  # HELLO_ACK and everything after it is framed at the chosen limit
        self._state = State.READY
        self._sent_flags = flags  # HELLO_ACK announces them; the first change goes at once
        return self._queue_message(Op.HELLO_ACK, 0, ack)

    def _open(self, stream: int, b: bytes, now: int) -> bool:
        """Admission, firmware-009 §6.2 #4 (console) and spec R2 (relay)."""
        if stream <= self._highest:
            return self._fail(Error.BAD_PROTOCOL)  # ids only increase; never revive one
        limit, port, kind, host = _get32(b, 0), _get16(b, 4), b[6], b[8:]
        # `limit > WINDOW` is stricter than §6.2 #4 (which names only limit 0 and port 0): the
        # P4 only ever offers 1024 (relay) or 4096 (console), so no P4 reaches this refusal.
        if not 1 <= limit <= WINDOW or not port:
            return self._fail(Error.BAD_PROTOCOL)
        if kind == AddressType.LOCAL:
            if host:
                return self._fail(Error.BAD_PROTOCOL)
            self._admitted(stream)
            if port != CONSOLE_PORT:
                return self._refuse(stream, Error.UNSUPPORTED)
            if not self.local_flags & CONSOLE_WANTED:
                return self._refuse(stream, Error.SERVICE_OFF)
            if self._slots[CONSOLE_SLOT] is not None:
                return self._refuse(stream, Error.BUSY)
            return self._open_console(stream, limit, now)
        target = RelayTarget.from_wire(kind, host, port)
        if target is None:
            return self._fail(Error.BAD_PROTOCOL)
        self._admitted(stream)
        if not self.local_flags & RELAY_ON:
            return self._refuse(stream, Error.SERVICE_OFF)
        slot = next((i for i in range(RELAY_SLOTS) if self._slots[i] is None), None)
        if slot is None:
            return self._refuse(stream, Error.BUSY)
        return self._open_relay(slot, stream, target, limit, now)

    def _admitted(self, stream: int) -> None:
        self._highest = stream
        self.stats.opens += 1

    def _open_console(self, stream: int, limit: int, now: int) -> bool:
        if not self._queue_message(Op.OPEN_OK, stream, le32(CONSOLE_LIMIT)):
            return False
        self._slots[CONSOLE_SLOT] = _Stream(stream, StreamKind.CONSOLE, StreamState.OPEN, now, peer_limit=limit,
                                            granted=CONSOLE_LIMIT, grant_at=now)
        if self._console is not None:
            self._console.on_console_open(stream)
        return self._state is not State.ENDED

    def _open_relay(self, slot: int, stream: int, target: RelayTarget, limit: int, now: int) -> bool:
        s = _Stream(stream, StreamKind.RELAY, StreamState.CONNECTING, now, target=target, peer_limit=limit)
        self._slots[slot] = s
        assert self._relay is not None  # RELAY_ON needs an endpoint
        answer = self._relay.relay_open(stream, target, now)
        if self._state is not State.READY or self._find(stream) is not s or s.state is not StreamState.CONNECTING:
            return self._state is not State.ENDED  # the endpoint already answered through the session
        if answer is CONNECTING:
            return True
        if answer == ACCEPT:
            return self.relay_connected(stream, now) or self._state is not State.ENDED
        if not known_error(answer):
            raise ValueError(f"relay_open answered {answer!r}")
        return self._local_end(s, answer)

    def _refuse(self, stream: int, code: int) -> bool:
        self.stats.refused[code] = self.stats.refused.get(code, 0) + 1
        if code == Error.SERVICE_OFF:
            self._link_forced = True  # §6.2 #3: tell the P4 our current flags
        return self._queue_message(Op.OPEN_ERR, stream, le16(code))

    def _local_end(self, s: _Stream, code: int) -> bool:
        """Our end of a stream: OPEN_ERR while the P4 has not seen OPEN_OK start, RESET after."""
        ok_started = s.ok_sent or self._started_message(Op.OPEN_OK, s.stream)
        self._purge(s.stream)
        if ok_started:
            self.stats.local_resets[code] = self.stats.local_resets.get(code, 0) + 1
            self._end_stream(s, EndHow.LOCAL_RESET, code)
            return self._queue_message(Op.RESET, s.stream, le16(code))
        self._end_stream(s, EndHow.REFUSED, code)
        return self._refuse(s.stream, code)

    # ---- timers ---------------------------------------------------------------------------

    def tick(self, now: int) -> bool:
        """Deadlines, LINK_STATE, relay connect timeouts and FIN after drain. False once over."""
        if self._state is State.ENDED:
            return False
        if self._state is State.AWAIT_HELLO and now - self._started >= HELLO_DEADLINE_MS:
            return self._fail(Error.DEADLINE)
        if self._assembling and now - self._assembly_at >= REASSEMBLY_DEADLINE_MS:
            return self._fail(Error.DEADLINE)
        if self._state is not State.READY:
            return True
        if not self._link_state(now):
            return False
        for s in self._live():
            if s.state is StreamState.CONNECTING and now - s.opened_at >= CONNECT_DEADLINE_MS:
                if not self._local_end(s, Error.CONNECT_TIMEOUT):
                    return False
            elif s.state is StreamState.CLOSING and s.close_sent_at is not None and \
                    now - s.close_sent_at >= CLOSE_ACK_DEADLINE_MS:
                return self._fail(Error.DEADLINE)
            elif s.state is StreamState.OPEN and s.ok_sent and s.local_eof and not s.fin_queued and not s.up:
                s.fin_queued = True
                if not self._queue_message(Op.FIN, s.stream, le32(s.sent)):
                    return False
        if self._ping_pending and now - self._ping_at >= PONG_DEADLINE_MS:
            return self._fail(Error.DEADLINE)
        if now - self._last_rx >= SILENCE_MS:
            return self._fail(Error.DEADLINE)
        return True

    def _link_state(self, now: int) -> bool:
        """Mirror of the P4's linkState(): on change (or forced), at most one per second measured
        from queue time; an unsent one is overwritten in place."""
        flags = self.local_flags
        if flags == self._sent_flags and not self._link_forced:
            return True
        body = le32(flags)
        if (m := self._unstarted(Op.LINK_STATE, 0)) is not None:
            m.body = body
        else:
            if self._link_sent and now - self._link_at < LINK_STATE_SPACING_MS:
                return True
            if not self._queue_message(Op.LINK_STATE, 0, body):
                return False
            self._link_sent, self._link_at = True, now
        self._sent_flags, self._link_forced = flags, False
        return True

    # ---- phone -> P4 ----------------------------------------------------------------------

    def peek(self, now: int) -> bytes | None:
        """The next inner frame to seal and notify, or None when idle. The same frame comes back
        until :meth:`accept`. A fresh frame first applies the send-opportunity rules (credit and
        the relay PING), then takes the oldest control message, then round-robin DATA."""
        if self._pending is not None:
            return self._pending.frame
        if self._raw is not None:
            self._pending = _Pending(self._raw, control=True, stream=0, size=len(self._raw) - HEADER, raw=True)
            return self._raw
        if self._state not in (State.READY, State.ENDING):
            return None
        if self._tx_seq >= U32_MAX:
            self._fail(Error.BAD_PROTOCOL)
            return None
        if self._state is State.READY and not self._service(now):
            return None
        room = self._value - HEADER
        if self._queue:
            m = self._queue[0]
            n = min(len(m.body) - m.offset, room)
            flags = (FIRST if m.offset == 0 else 0) | (LAST if m.offset + n == len(m.body) else 0)
            body = m.body[m.offset:m.offset + n]
            self._pending = _Pending(build_frame(m.op, m.stream, self._tx_seq, body, flags), True, m.stream, n)
        elif self._state is State.READY and (s := self._next_data()) is not None:
            n = min(room, s.peer_limit - s.sent, len(s.up))
            body = bytes(s.up[:n])
            self._pending = _Pending(build_frame(Op.DATA, s.stream, self._tx_seq, body, 0), False, s.stream, n)
        else:
            return None
        return self._pending.frame

    def accept(self, now: int) -> bool:
        """The stack took the frame from :meth:`peek`: consume its sequence number and bytes.
        False when nothing was pending (or the session failed on it)."""
        p = self._pending
        if p is None:
            return False
        self._pending = None
        self._tx_seq += 1
        self.stats.frames_out += 1
        if p.raw:
            self._raw = None
            self._finish(self._end or SessionEnd(EndKind.VERSION, Error.UNSUPPORTED_VERSION))
            return True
        if not p.control:
            s = self._find(p.stream)
            if s is not None and s.state is StreamState.OPEN:  # a stream that ended meanwhile drops nothing
                if len(s.up) < p.size:
                    return self._fail(Error.BAD_PROTOCOL)
                s.up[:p.size] = bytes(p.size)
                del s.up[:p.size]
                s.sent += p.size
            return True
        m = self._queue[0]
        m.offset += p.size
        if m.offset < len(m.body):
            return True
        self._queue.pop(0)
        self._message_sent(m, now)
        return True

    def _message_sent(self, m: _Message, now: int) -> None:
        s = self._find(m.stream) if m.stream else None
        match m.op:
            case Op.HELLO_ACK:
                self._hello_ack_sent = True
            case Op.OPEN_OK if s is not None and s.state is StreamState.OPEN:
                s.ok_sent = True
                s.receive_limit = _get32(m.body, 0)
            case Op.CREDIT if s is not None and s.state is StreamState.OPEN:
                s.receive_limit = _get32(m.body, 0)
            case Op.CLOSE if s is not None and s.state is StreamState.CLOSING:
                s.close_sent_at = now
            case Op.LINK_STATE:
                self.stats.link_states_out += 1
            case Op.PING:
                self.stats.pings_out += 1
            case Op.RESET if m.stream == 0 and self._state is State.ENDING:
                self._finish(self._end or SessionEnd(EndKind.LOCAL_RESET, _get16(m.body, 0)))

    def failure_frame(self) -> bytes | None:
        """After a FAILED end: a best-effort stream-0 RESET with our error, at the next sequence
        number, to seal and send before disconnecting. None when nothing is safe to send (the P4
        ended it, HELLO_ACK never went out, or another kind of end)."""
        e = self._end
        if (self._state is not State.ENDED or e is None or e.kind is not EndKind.FAILED
                or not self._hello_ack_sent or self._tx_seq >= U32_MAX):
            return None
        return build_frame(Op.RESET, 0, self._tx_seq, le16(e.error))

    def _service(self, now: int) -> bool:
        """The send-opportunity rules: credit for every open stream, by id, then the relay PING."""
        relay_open = False
        for s in sorted(self._live(), key=lambda x: x.stream):
            if s.state is not StreamState.OPEN:
                continue
            relay_open |= s.kind is StreamKind.RELAY
            if s.peer_fin:
                continue  # the P4 sends nothing more on it
            if s.consumed > U32_MAX - WINDOW:
                return self._fail(Error.FLOW_VIOLATION)
            limit = s.consumed + WINDOW
            if limit > s.granted and (limit - s.granted >= CREDIT_BYTES or now - s.grant_at >= CREDIT_MS):
                if not self._queue_message(Op.CREDIT, s.stream, le32(limit)):
                    return False
                s.granted, s.grant_at = limit, now
        if relay_open and not self._ping_pending and now - self._last_rx >= PING_IDLE_MS:
            return self._queue_ping(now)
        return True

    def _queue_ping(self, now: int) -> bool:
        cookie = self._random(4)
        if len(cookie) != 4:
            raise RuntimeError("random source returned the wrong size")
        self._ping_cookie, self._ping_at, self._ping_pending = cookie, now, True
        return self._queue_message(Op.PING, 0, cookie)

    def _next_data(self) -> _Stream | None:
        """Round-robin by stream id: the next id after the last sender that can send."""
        eligible = [s for s in self._live() if s.state is StreamState.OPEN and s.ok_sent and s.up
                    and s.peer_limit > s.sent]
        if not eligible:
            return None
        later = [s for s in eligible if s.stream > self._rr]
        chosen = min(later or eligible, key=lambda x: x.stream)
        self._rr = chosen.stream
        return chosen

    # ---- the control queue ----------------------------------------------------------------

    def _started_index(self, i: int) -> bool:
        p = self._pending
        return self._queue[i].offset > 0 or (i == 0 and p is not None and p.control and not p.raw)

    def _started_message(self, op: Op, stream: int) -> bool:
        return any(m.op is op and m.stream == stream and self._started_index(i) for i, m in enumerate(self._queue))

    def _unstarted(self, op: Op, stream: int) -> _Message | None:
        for i, m in enumerate(self._queue):
            if m.op is op and m.stream == stream and not self._started_index(i):
                return m
        return None

    def _queue_message(self, op: Op, stream: int, body: bytes) -> bool:
        """Append a control message. CREDIT (per stream), LINK_STATE and PONG are snapshots: an
        unstarted one is overwritten in place and keeps its position. A 13th entry fails the
        session with RESOURCES (10)."""
        if len(body) > CONTROL_CAP:
            return self._fail(Error.BAD_PROTOCOL)
        if op is Op.CREDIT or (stream == 0 and op in (Op.LINK_STATE, Op.PONG)):
            if (m := self._unstarted(op, stream)) is not None:
                m.body = bytes(body)
                return True
        if len(self._queue) >= CONTROL_QUEUE:
            return self._fail(Error.NO_RESOURCES)
        self._queue.append(_Message(op, stream, bytes(body)))
        return True

    def _purge(self, stream: int, ops: tuple[Op, ...] | None = None) -> None:
        """Drop the stream's unstarted messages (all, or only ``ops``)."""
        self._queue = [m for i, m in enumerate(self._queue)
                       if m.stream != stream or self._started_index(i) or (ops is not None and m.op not in ops)]

    # ---- streams and endings --------------------------------------------------------------

    def _live(self) -> list[_Stream]:
        return [s for s in self._slots if s is not None]

    def _find(self, stream: int) -> _Stream | None:
        if not stream:
            return None
        return next((s for s in self._slots if s is not None and s.stream == stream), None)

    def _relay_stream(self, stream: int) -> _Stream | None:
        s = self._find(stream)
        return s if s is not None and s.kind is StreamKind.RELAY else None

    def _retired(self, stream: int) -> bool:
        return 0 < stream <= self._highest and self._find(stream) is None

    def _end_stream(self, s: _Stream, how: EndHow, code: int) -> None:
        index = self._slots.index(s)
        self._slots[index] = None
        _wipe(s.up)
        _wipe(s.down)
        end = StreamEnd(s.stream, how, code)
        if s.kind is StreamKind.CONSOLE:
            if self._console is not None:
                self._console.on_console_end(end)
        elif self._relay is not None:
            self._relay.relay_end(s.stream, end)

    def _send_version_signal(self) -> bool:
        """The phone's first inner frame becomes the v1-header stream-0 RESET 2 (§5.4)."""
        assert self._tx_seq == 0 and not self._queue
        self._raw = VERSION_RESET
        self._state = State.ENDING
        self._end = SessionEnd(EndKind.VERSION, Error.UNSUPPORTED_VERSION)
        return True

    def _fail(self, error: int) -> bool:
        """A violation by the P4 (or a local limit): the session fails; the first cause wins."""
        if self._state is not State.ENDED:
            self._finish(SessionEnd(EndKind.FAILED, error))
        return False

    def _finish(self, end: SessionEnd) -> bool:
        """End the session: every stream ends, the queues and buffers are dropped."""
        self._state = State.ENDED
        self._end = end
        self._queue.clear()
        self._pending = None
        self._raw = None
        self._assembling = False
        _wipe(self._assembly)
        self._ping_pending = False
        for s in self._live():
            self._end_stream(s, EndHow.SESSION, end.error)
        return False
