"""Shared test plumbing for the inner-protocol tests (test_inner, test_x1, test_x2).

A :class:`Link` is one phone-side session the way the GATT layer drives it: the
lane-A :class:`~hw1peer.channel.PhoneChannel` (outer layer), an
:class:`~hw1peer.inner.Session`, a :class:`~hw1peer.inner.TxGate` and a
millisecond clock the test advances. P4 values go in through :meth:`Link.p4`;
:meth:`Link.step` runs the session's tick and, when the gate allows, seals and
"notifies" one frame, recording it in :attr:`Link.sent`.

The fakes behave like the X1 fixture's scripted phone (``phone_rules``): the
console sink and the relay take every P4 byte at once.
"""

from collections.abc import Callable
from dataclasses import dataclass, field

from hw1peer import inner
from hw1peer.channel import Data, Established, PhoneChannel, Reply
from hw1peer.inner import (CONNECTING, RelayTarget, Session, StreamEnd, TxGate)
from hw1peer.tests import load_fixture, unhex

X1 = "phone_pipe_v2_sessions.json"
X2 = "phone_pipe_v2_negative.json"
#: The X1 phone's pipe HELLO_ACK nonce (the fixture's pipe_nonces.phone_hex).
PHONE_PIPE_NONCE = bytes(range(0x11, 0x19))


def fixed_random(nonce: bytes = PHONE_PIPE_NONCE, cookies: list[bytes] | None = None) -> Callable[[int], bytes]:
    """A random source for replay: ``nonce`` for 8-byte draws (HELLO_ACK), then the queued
    ``cookies`` (or 70 69 6e NN, the generator's counter) for 4-byte draws (PING)."""
    queue = list(cookies or [])
    counter = [0x70696E00]

    def draw(size: int) -> bytes:
        if size == 8:
            return nonce
        if size == 4:
            if queue:
                return queue.pop(0)
            counter[0] += 1
            return counter[0].to_bytes(4, "little")
        raise AssertionError(f"unexpected draw of {size} bytes")
    return draw


class RecordingConsole:
    """A console sink that keeps everything (and can call a record malformed)."""

    def __init__(self) -> None:
        self.opens: list[int] = []
        self.bytes: dict[int, bytearray] = {}
        self.ends: list[StreamEnd] = []
        self.current = 0
        self.malformed_at: int | None = None  # answer False once this many bytes have arrived

    def on_console_open(self, stream: int) -> None:
        self.opens.append(stream)
        self.bytes[stream] = bytearray()
        self.current = stream

    def on_console_bytes(self, data: bytes) -> bool:
        received = self.bytes[self.current]
        received += data
        return self.malformed_at is None or len(received) < self.malformed_at

    def on_console_end(self, end: StreamEnd) -> None:
        self.ends.append(end)
        self.current = 0


class ScriptedRelay:
    """A relay endpoint: answers each OPEN from ``answers`` (default CONNECTING), reads every
    P4 byte at once and records what it got."""

    def __init__(self, answers: dict[int, int | None] | None = None) -> None:
        self.session: Session | None = None
        self.answers = answers or {}
        self.opens: list[tuple[int, RelayTarget]] = []
        self.received: dict[int, bytearray] = {}
        self.fins: list[int] = []
        self.ends: list[StreamEnd] = []

    def relay_open(self, stream: int, target: RelayTarget, now: int) -> int | None:
        self.opens.append((stream, target))
        self.received[stream] = bytearray()
        return self.answers.get(stream, CONNECTING)

    def relay_data(self, stream: int) -> None:
        assert self.session is not None
        self.received[stream] += self.session.relay_read(stream)
        if self.session.relay_shutdown_due(stream):
            self.fins.append(stream)
            self.session.relay_output_shut(stream)

    def relay_end(self, stream: int, end: StreamEnd) -> None:
        self.ends.append(end)


@dataclass
class Sent:
    t: int
    value: bytes
    inner: bytes


@dataclass
class Link:
    """One phone-side link: channel + session + gate, on a test clock."""

    channel: PhoneChannel
    session: Session
    console: RecordingConsole
    relay: ScriptedRelay
    gate: TxGate = field(default_factory=TxGate)
    sent: list[Sent] = field(default_factory=list)

    def p4(self, value: bytes, now: int) -> bool:
        """One P4 value: open it with the channel and hand the inner frame to the session."""
        rx = self.channel.receive(value)
        assert isinstance(rx, Data), rx
        return self.session.receive(rx.plaintext, now)

    def p4_inner(self, frame: bytes, now: int) -> bool:
        """A plaintext inner frame straight to the session (no outer layer)."""
        return self.session.receive(frame, now)

    def pump(self, now: int) -> Sent | None:
        """At most one notification, when the gate allows (sealed, recorded, accepted)."""
        if not self.gate.ready(now):
            return None
        frame = self.session.peek(now)
        if frame is None:
            return None
        value = self.channel.seal(frame)
        assert value is not None
        self.gate.submitted(now)
        assert self.session.accept(now)
        self.gate.completed()
        sent = Sent(now, value, frame)
        self.sent.append(sent)
        return sent

    def step(self, now: int) -> Sent | None:
        self.session.tick(now)
        return self.pump(now)

    def run(self, start: int, stop: int) -> list[Sent]:
        """Step every millisecond in [start, stop)."""
        before = len(self.sent)
        for t in range(start, stop):
            self.step(t)
        return self.sent[before:]

    def frames_out(self) -> list[inner.Frame]:
        out = [inner.Frame.parse(s.inner) for s in self.sent]
        assert all(f is not None for f in out)
        return out  # type: ignore[return-value]


def x1_session(mtu: int) -> dict:
    return next(s for s in load_fixture(X1)["sessions"] if s["att_mtu"] == mtu)


def make_link(mtu: int, now: int, *, relay_on: bool = True, console_wanted: bool = True,
              answers: dict[int, int | None] | None = None, mismatch: bool = False,
              random: Callable[[int], bytes] | None = None) -> Link:
    """A link whose outer channel went through X1's handshake at ``mtu`` (frames 1-4), with a
    fresh Session started at ``now``. The gate remembers the CONFIRM notification (frame 3)."""
    x1 = load_fixture(X1)
    frames = x1_session(mtu)["frames"]
    channel = PhoneChannel(unhex(x1["psk_hex"]), private_key=unhex(x1["phone_private_key_hex"]),
                           nonce=unhex(x1["phone_nonce_hex"]))
    assert channel.hello() == unhex(frames[1]["value_hex"])
    assert channel.receive(unhex(frames[2]["value_hex"])) == Reply(unhex(frames[3]["value_hex"]))
    assert channel.receive(unhex(frames[4]["value_hex"])) == Established()
    console, relay = RecordingConsole(), ScriptedRelay(answers)
    session = Session(inner.frame_limit(mtu), now, console=console, relay=relay, relay_on=relay_on,
                      console_wanted=console_wanted, mismatch=mismatch, random=random or fixed_random())
    relay.session = session
    gate = TxGate()
    gate.last_at = frames[3]["t_ms"]
    return Link(channel, session, console, relay, gate)


def pipe_base(*, through_hello_ack: bool = True, **options) -> Link:
    """X2's base point on the MTU-247 X1 session: the P4 HELLO (frame 5) delivered and, with
    ``through_hello_ack``, the phone HELLO_ACK (frame 6) sent and checked byte for byte."""
    frames = x1_session(247)["frames"]
    link = make_link(247, frames[4]["t_ms"], **options)
    if through_hello_ack:
        hello, ack = frames[5], frames[6]
        assert link.p4(unhex(hello["value_hex"]), hello["t_ms"])
        sent = link.step(ack["t_ms"])
        assert sent is not None and sent.value == unhex(ack["value_hex"]), "HELLO_ACK differs from X1 frame 6"
        link.sent.clear()  # the base point: what follows is the vector's
    return link


def parse_records(data: bytes) -> list[tuple[int, int, bytes, int]]:
    """Console records as (type, flags, payload, offset) from a byte stream (whole records)."""
    out, at = [], 0
    while at + 4 <= len(data):
        length = data[at + 2] | data[at + 3] << 8
        if at + 4 + length > len(data):
            break
        out.append((data[at], data[at + 1], bytes(data[at + 4:at + 4 + length]), at))
        at += 4 + length
    assert at == len(data), "trailing partial record"
    return out
