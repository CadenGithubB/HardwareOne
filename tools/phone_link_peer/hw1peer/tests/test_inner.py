"""inner: the phone side of inner protocol v2, rule by rule (plaintext frames, no fixtures).

The X1/X2 replays (test_x1, test_x2) pin the bytes; these tests pin each rule
on its own: sizes and framing, HELLO, the version signal, admission order
(§6.2 #4), LINK_STATE out and in, credit and flow, DATA scheduling, CLOSE on
live, refused and reset ids, RESET, FIN, PING/PONG and silence, the 12-deep
queue, failure frames, and the local controls.
"""

from hw1peer import inner
from hw1peer.inner import (ACCEPT, CONNECTING, WHOLE, AddressType, EndHow, EndKind, Error, Frame, Op, RelayTarget,
                           Session, SessionEnd, State, StreamEnd, StreamKind, StreamState, TxGate, build_frame, le16,
                           le32)
from hw1peer.tests.pipe_harness import RecordingConsole, ScriptedRelay, fixed_random

P4_NONCE = bytes(range(1, 9))
IPV4 = bytes([192, 0, 2, 10])


class P4:
    """Builds P4 frames with contiguous sequence numbers and hands them to a session."""

    def __init__(self, session: Session) -> None:
        self.s = session
        self.seq = 0
        self.now = 0

    def send(self, op: int, stream: int = 0, body: bytes = b"", flags: int = WHOLE, version: int = 2,
             seq: int | None = None) -> bool:
        frame = build_frame(op, stream, self.seq if seq is None else seq, body, flags, version)
        self.seq += 1
        return self.s.receive(frame, self.now)

    def hello(self, flags: int = 1, offer: int = 219, nonce: bytes = P4_NONCE, streams: int = 3) -> bool:
        return self.send(Op.HELLO, 0, nonce + bytes((streams, flags)) + le16(offer))

    def open_console(self, stream: int, limit: int = 4096, port: int = 1, host: bytes = b"") -> bool:
        return self.send(Op.OPEN, stream, le32(limit) + le16(port) + bytes((4, len(host))) + host)

    def open_relay(self, stream: int, kind: int = 1, host: bytes = IPV4, port: int = 443, limit: int = 1024) -> bool:
        return self.send(Op.OPEN, stream, le32(limit) + le16(port) + bytes((kind, len(host))) + host)


def session(max_frame: int = 219, **options) -> tuple[Session, RecordingConsole, ScriptedRelay]:
    console, relay = RecordingConsole(), ScriptedRelay(options.pop("answers", None))
    options.setdefault("relay_on", True)
    options.setdefault("console_wanted", True)
    options.setdefault("random", fixed_random())
    s = Session(max_frame, 0, console=console, relay=relay, **options)
    relay.session = s
    return s, console, relay


def drain(s: Session, now: int = 0, limit: int = 100) -> list[Frame]:
    """Every frame the session will send now (ignoring the gate)."""
    out = []
    for _ in range(limit):
        frame = s.peek(now)
        if frame is None:
            return out
        assert s.accept(now)
        parsed = Frame.parse(frame)
        assert parsed is not None
        out.append(parsed)
    raise AssertionError("the session never went idle")


def ready(**options) -> tuple[Session, P4, RecordingConsole, ScriptedRelay]:
    """A READY session whose HELLO_ACK went out."""
    s, console, relay = session(**options)
    p4 = P4(s)
    assert p4.hello()
    assert [f.op for f in drain(s)] == [Op.HELLO_ACK]
    return s, p4, console, relay


def ops(frames: list[Frame]) -> list[tuple[int, int, bytes]]:
    return [(f.op, f.stream, f.body) for f in frames]


def failed(s: Session, error: int = Error.BAD_PROTOCOL) -> bool:
    return s.done and s.end == SessionEnd(EndKind.FAILED, error)


def raises(call, kind=ValueError) -> bool:
    try:
        call()
    except kind:
        return True
    return False


# ---- sizes, frames, targets -----------------------------------------------------------------

def test_sizes():
    assert [inner.value_limit(m) for m in (22, 23, 64, 247, 515, 517)] == [0, 20, 61, 244, 512, 512]
    assert [inner.frame_limit(m) for m in (23, 63, 64, 244, 247, 517)] == [0, 0, 36, 216, 219, 487]
    assert inner.frame_limit_for_update_length(244) == 219 and inner.frame_limit_for_update_length(60) == 0
    assert inner.CONTROL_QUEUE == 3 * inner.MAX_STREAMS + 3 and inner.known_error(18) and not inner.known_error(19)
    assert raises(lambda: Session(19, 0)) and raises(lambda: Session(513, 0)) and Session(20, 0).value == 20


def test_frame_build_and_parse():
    f = build_frame(Op.CREDIT, 7, 0x01020304, le32(4096))
    assert f.hex() == "0221" "0700" "04030201" "0400" "03" "00" "00100000"
    parsed = Frame.parse(f)
    assert parsed == Frame(2, Op.CREDIT, 7, 0x01020304, WHOLE, le32(4096))
    assert Frame.parse(f[:-1]) is None and Frame.parse(f[:11]) is None
    assert inner.VERSION_RESET == build_frame(Op.RESET, 0, 0, le16(2), version=1)
    assert inner.VERSION_RESET.hex() == "0125000000000000020003000200"


def test_relay_target_from_wire():
    t = RelayTarget.from_wire(1, IPV4, 443)
    assert t is not None and t.host_text() == "192.0.2.10" and "192" not in repr(t) and "443" not in repr(t)
    v6 = RelayTarget.from_wire(2, bytes.fromhex("20010db8000000000000000000000010"), 443)
    assert v6 is not None and v6.host_text() == "2001:db8::10"
    assert RelayTarget.from_wire(3, b"relay.example.test", 8443).host_text() == "relay.example.test"
    for kind, host, port in ((1, IPV4[:3], 443), (2, IPV4, 443), (3, b"", 443), (3, b"a" * 254, 443),
                             (3, b"Relay.test", 443), (3, b"a_b", 443), (1, IPV4, 0), (4, b"", 1), (5, IPV4, 1)):
        assert RelayTarget.from_wire(kind, host, port) is None, (kind, host, port)
    assert RelayTarget.from_wire(3, b"a" * 253, 1) is not None


def test_tx_gate():
    g = TxGate()
    assert g.ready(0)
    g.submitted(10)
    assert not g.ready(100)
    g.completed()
    assert not g.ready(39) and g.ready(40)
    g.reset()
    assert g.ready(0)


# ---- HELLO, the version signal, the HELLO stage ----------------------------------------------

def test_hello_ack_layout():
    s, _, _ = session(max_frame=219)
    assert P4(s).hello(flags=1, offer=200)
    (ack,) = drain(s)
    assert (ack.op, ack.stream, ack.seq, ack.flags) == (Op.HELLO_ACK, 0, 0, WHOLE)
    assert ack.body == P4_NONCE + bytes(range(0x11, 0x19)) + bytes((3, 3)) + le16(200)
    assert s.value == 200 and s.ready and s.hello_ack_sent and s.console_offered and s.device_nonce == P4_NONCE
    s, _, _ = session(max_frame=36, relay_on=False)  # chosen = min(P, offer)
    assert P4(s).hello(offer=219)
    assert drain(s)[0].body[16:] == bytes((3, 2)) + le16(36)


def test_hello_rejected():
    for kwargs in ({"nonce": bytes(8)}, {"streams": 2}, {"flags": 2}, {"flags": 0x81}, {"offer": 19},
                   {"offer": 513}):
        s, _, _ = session()
        assert not P4(s).hello(**kwargs) and failed(s), kwargs
        assert s.failure_frame() is None  # no HELLO_ACK went out
    s, _, _ = session()
    assert not P4(s).send(Op.HELLO, 0, P4_NONCE + bytes((3, 1)) + le16(219) + b"\0") and failed(s)


def test_hello_nonce_retries_and_gives_up():
    draws = iter([bytes(8), bytes(8), bytes(range(1, 9))])
    s = Session(219, 0, random=lambda n: next(draws))
    assert P4(s).hello() and drain(s)[0].body[8:16] == bytes(range(1, 9))
    s = Session(219, 0, random=lambda n: bytes(n))
    assert not P4(s).hello() and failed(s, Error.NO_RESOURCES)


def test_only_hello_or_reset_before_ready():
    for op, stream, body in ((Op.LINK_STATE, 0, le32(1)), (Op.PING, 0, le32(1)), (Op.PONG, 0, le32(1)),
                             (Op.HELLO_ACK, 0, bytes(20)), (Op.OPEN, 1, le32(4096) + le16(1) + bytes((4, 0))),
                             (Op.DATA, 1, b"x")):
        s, _, _ = session()
        p4 = P4(s)
        assert not p4.send(op, stream, body, 0 if op == Op.DATA else WHOLE) and failed(s), op
    s, _, _ = session()
    assert not P4(s).send(Op.RESET, 0, le16(Error.PAUSED))
    assert s.end == SessionEnd(EndKind.PEER_RESET, Error.PAUSED) and s.failure_frame() is None


def test_version_signal_for_any_other_version():
    for version in (0, 1, 3, 0xFF):
        s, _, _ = session()
        assert P4(s).send(Op.HELLO, 0, P4_NONCE + bytes((2, 0)) + le16(219), version=version)
        assert s.state is State.ENDING and [f for f in [s.peek(0)]] == [inner.VERSION_RESET]
        assert s.accept(0) and s.done and s.end.strike and s.end.error == Error.UNSUPPORTED_VERSION
        assert s.peek(1) is None and s.failure_frame() is None and not s.hello_ack_sent


def test_mismatch_mode_answers_a_v2_hello_with_the_signal():
    s, _, _ = session(mismatch=True)
    assert P4(s).hello()
    assert s.peek(0) == inner.VERSION_RESET and s.accept(0) and s.end.kind is EndKind.VERSION
    s, p4, _, _ = ready()
    s.mismatch = True  # only the HELLO answer looks at it
    assert p4.send(Op.PING, 0, le32(5)) and ops(drain(s)) == [(Op.PONG, 0, le32(5))]


def test_hello_deadline():
    s, _, _ = session()
    assert s.tick(2999) and not s.tick(3000) and failed(s, Error.DEADLINE)
    s, _, _ = session()
    assert P4(s).hello() and s.tick(10_000)  # READY: no HELLO deadline


def test_frames_while_ending_are_ignored():
    s, p4, _, _ = ready()
    assert s.end_session()
    assert p4.send(Op.PING, 0, le32(1)) and p4.send(Op.LINK_STATE, 0, le32(7)) and s.state is State.ENDING
    assert ops(drain(s)) == [(Op.RESET, 0, le16(Error.PAUSED))] and s.done
    assert not p4.send(Op.PING, 0, le32(2))


# ---- header and reassembly ----------------------------------------------------------------

def test_header_violations():
    cases = [
        lambda p: p.send(Op.PING, 0, le32(1), seq=5),                              # sequence gap
        lambda p: p.s.receive(build_frame(Op.PING, 0, p.seq, le32(1))[:-1], 0),   # length field
        lambda p: p.s.receive(build_frame(Op.PING, 0, p.seq, le32(1))[:11] + b"\1" + le32(1), 0),  # byte 11
        lambda p: p.s.receive(build_frame(Op.DATA, 1, p.seq, bytes(208), 0), 0),  # over the value (219)
        lambda p: p.send(0x33, 0, le32(1)),                                        # unknown op
        lambda p: p.send(Op.PING, 0, le32(1), version=1),                         # v1 after READY
        lambda p: p.send(Op.HELLO, 0, P4_NONCE + bytes((3, 1)) + le16(219)),      # HELLO again
        lambda p: p.send(Op.DATA, 0, b"x", 0),                                     # DATA on stream 0
        lambda p: p.send(Op.DATA, 9, b"x", 0),                                     # DATA for an id never opened
        lambda p: p.send(Op.OPEN_OK, 1, le32(1)),                                  # the phone never opens
        lambda p: p.send(Op.OPEN_ERR, 1, le16(9)),
        lambda p: p.send(Op.PING, 1, le32(1)),                                     # PING on a stream
        lambda p: p.send(Op.CREDIT, 9, le32(1)),                                   # id never opened
    ]
    for k, case in enumerate(cases):
        s, p4, _, _ = ready()
        assert not case(p4) and failed(s), k
        assert s.failure_frame() == build_frame(Op.RESET, 0, 1, le16(Error.BAD_PROTOCOL)), k


def test_reassembly_of_a_fragmented_open():
    s, p4, _, relay = ready()
    name = b"relay.example.test"
    body = le32(1024) + le16(8443) + bytes((3, len(name))) + name
    assert p4.send(Op.OPEN, 4, body[:24], inner.FIRST) and s.streams() == []
    assert p4.send(Op.OPEN, 4, body[24:], inner.LAST)
    (stream, target), = relay.opens
    assert stream == 4 and target.host_text() == "relay.example.test" and target.port == 8443


def test_reassembly_violations():
    open_body = le32(1024) + le16(443) + bytes((1, 4)) + IPV4
    cases = [
        lambda p: p.send(Op.OPEN, 1, open_body, inner.LAST),                                       # LAST alone
        lambda p: p.send(Op.OPEN, 1, open_body[:4], 0),                                            # middle alone
        lambda p: p.send(Op.OPEN, 1, open_body[:4], inner.FIRST) and p.send(Op.OPEN, 1, open_body[4:], inner.FIRST),
        lambda p: p.send(Op.OPEN, 1, open_body[:4], inner.FIRST) and p.send(Op.CREDIT, 1, open_body[4:], inner.LAST),
        lambda p: p.send(Op.OPEN, 1, open_body[:4], inner.FIRST) and p.send(Op.OPEN, 2, open_body[4:], inner.LAST),
        lambda p: p.send(Op.OPEN, 1, open_body[:4], inner.FIRST) and p.send(Op.DATA, 1, b"x", 0),
        lambda p: p.send(Op.OPEN, 1, open_body[:4], inner.FIRST) and p.send(Op.OPEN, 1, b"", 0),  # empty middle
        lambda p: p.send(Op.PING, 0, le32(1), 0x07),                                               # flag bit 2
        lambda p: all(p.send(Op.OPEN, 1, bytes(200), f) for f in (inner.FIRST, 0)),               # over 272
    ]
    for k, case in enumerate(cases):
        s, p4, _, _ = ready()
        assert not case(p4) and failed(s), k


def test_reassembly_deadline():
    s, p4, _, _ = ready()
    assert p4.send(Op.OPEN, 1, le32(1024), inner.FIRST)
    assert s.tick(1999) and not s.tick(2000) and failed(s, Error.DEADLINE)


# ---- admission ---------------------------------------------------------------------------

def test_console_admission_order():
    """§6.2 #4: limit/port 0 BAD, type 4 len != 0 BAD, port != 1 -> 17, unwanted -> 16, second -> 9."""
    for kwargs in ({"limit": 0}, {"port": 0}, {"limit": 4097}, {"host": b"x"}):
        s, p4, _, _ = ready(console_wanted=False)
        assert not p4.open_console(1, **kwargs) and failed(s), kwargs
    s, p4, console, _ = ready(console_wanted=False)
    assert p4.open_console(1, port=2) and p4.open_console(2)
    assert ops(drain(s)) == [(Op.OPEN_ERR, 1, le16(Error.UNSUPPORTED)), (Op.OPEN_ERR, 2, le16(Error.SERVICE_OFF))]
    s, p4, console, _ = ready()
    assert p4.open_console(1) and p4.open_console(2)
    assert ops(drain(s)) == [(Op.OPEN_OK, 1, le32(4096)), (Op.OPEN_ERR, 2, le16(Error.BUSY))]
    assert console.opens == [1] and s.console_stream() == 1
    assert not p4.open_console(2) and failed(s)  # ids only increase


def test_console_wanted_needs_a_sink():
    s = Session(219, 0, console_wanted=True, random=fixed_random())
    p4 = P4(s)
    assert p4.hello() and drain(s)[0].body[17] == 0 and s.local_flags == 0
    assert p4.open_console(1) and ops(drain(s))[0] == (Op.OPEN_ERR, 1, le16(Error.SERVICE_OFF))


def test_relay_admission():
    s, p4, _, relay = ready(relay_on=False)
    assert p4.open_relay(1) and ops(drain(s))[0] == (Op.OPEN_ERR, 1, le16(Error.SERVICE_OFF)) and relay.opens == []
    s, p4, _, relay = ready()
    assert p4.open_relay(1) and p4.open_relay(2) and p4.open_relay(3)
    assert ops(drain(s)) == [(Op.OPEN_ERR, 3, le16(Error.BUSY))] and [i for i, _ in relay.opens] == [1, 2]
    assert [x.state for x in s.streams()] == [StreamState.CONNECTING] * 2
    assert s.relay_connected(1, 5) and not s.relay_connected(1, 5) and not s.relay_connected(3, 5)
    assert ops(drain(s, 5))[0] == (Op.OPEN_OK, 1, le32(1024))
    for host, kind in ((IPV4[:3], 1), (b"UPPER", 3), (IPV4, 9)):
        s, p4, _, _ = ready()
        assert not p4.open_relay(1, kind=kind, host=host) and failed(s), (host, kind)


def test_relay_endpoint_answers():
    s, p4, _, relay = ready(answers={1: ACCEPT, 2: Error.VPN_UNAVAILABLE, 3: CONNECTING})
    assert p4.open_relay(1) and p4.open_relay(2) and p4.open_relay(3)
    out = ops(drain(s))
    assert out[:2] == [(Op.OPEN_OK, 1, le32(1024)), (Op.OPEN_ERR, 2, le16(Error.VPN_UNAVAILABLE))]
    assert relay.ends == [StreamEnd(2, EndHow.REFUSED, Error.VPN_UNAVAILABLE)]
    assert s.stats.refused == {Error.VPN_UNAVAILABLE: 1} and s.stats.opens == 3

    class Synchronous(ScriptedRelay):
        def relay_open(self, stream, target, now):
            super().relay_open(stream, target, now)
            assert self.session.relay_connected(stream, now)
            return Error.TARGET_DENIED  # ignored: the endpoint already answered
    relay = Synchronous()
    s = Session(219, 0, relay=relay, relay_on=True, random=fixed_random())
    relay.session = s
    p4 = P4(s)
    assert p4.hello() and drain(s) and p4.open_relay(1)
    assert ops(drain(s))[0] == (Op.OPEN_OK, 1, le32(1024)) and s.streams()[0].state is StreamState.OPEN


def test_relay_connect_deadline():
    s, p4, _, relay = ready()
    p4.now = 100
    assert p4.open_relay(1)
    assert s.tick(2599) and drain(s, 2599) == []
    assert s.tick(2600) and ops(drain(s, 2600)) == [(Op.OPEN_ERR, 1, le16(Error.CONNECT_TIMEOUT))]
    assert relay.ends == [StreamEnd(1, EndHow.REFUSED, Error.CONNECT_TIMEOUT)] and not s.relay_connected(1, 2601)


# ---- LINK_STATE ---------------------------------------------------------------------------

def test_link_state_on_change_spaced_and_coalesced():
    s, p4, _, _ = ready()
    s.set_relay_on(False)
    assert s.tick(100) and ops(drain(s, 100)) == [(Op.LINK_STATE, 0, le32(2))]  # the first change at once
    s.set_console_wanted(False)
    assert s.tick(500) and drain(s, 500) == []                                   # within 1 s of the last
    s.set_relay_on(True)
    assert s.tick(1099) and drain(s, 1099) == []
    assert s.tick(1100) and ops(drain(s, 1100)) == [(Op.LINK_STATE, 0, le32(1))]  # the newest flags only
    s.set_relay_on(False)
    s.set_relay_on(True)  # a flap inside the second: nothing
    assert s.tick(2200) and drain(s, 2200) == [] and s.stats.link_states_out == 2


def test_link_state_overwritten_in_place():
    s, p4, _, _ = ready()
    assert p4.send(Op.PING, 0, le32(1))
    s.set_relay_on(False)
    assert s.tick(10)                       # queued behind the PONG, not yet sent
    s.set_console_wanted(False)
    s.set_relay_on(True)
    assert s.tick(20)                       # the unsent one takes the newest flags in place
    assert ops(drain(s, 20)) == [(Op.PONG, 0, le32(1)), (Op.LINK_STATE, 0, le32(1))]


def test_link_state_after_refusing_16_even_unchanged():
    """§6.2 #3: after a 16 the current flags go out even if coalescing would swallow them."""
    s, p4, _, _ = ready(answers={1: Error.SERVICE_OFF, 2: Error.SERVICE_OFF})
    assert p4.open_relay(1) and s.tick(1)
    assert ops(drain(s, 1)) == [(Op.OPEN_ERR, 1, le16(16)), (Op.LINK_STATE, 0, le32(3))]
    assert p4.open_relay(2) and s.tick(500)             # a second 16 within the second: spaced
    assert ops(drain(s, 500)) == [(Op.OPEN_ERR, 2, le16(16))]
    assert s.tick(1001) and ops(drain(s, 1001)) == [(Op.LINK_STATE, 0, le32(3))]


def test_link_state_in():
    s, p4, _, _ = ready()
    assert s.console_offered and p4.send(Op.LINK_STATE, 0, le32(0)) and s.p4_flags == 0
    assert p4.send(Op.LINK_STATE, 0, le32(1)) and s.console_offered and drain(s) == []  # no answer, no rate
    for body, stream in ((le32(3), 0), (le32(0x101), 0), (le16(1), 0), (le32(1), 1)):
        s, p4, _, _ = ready()
        assert not p4.send(Op.LINK_STATE, stream, body) and failed(s), (body, stream)


# ---- credit, flow and DATA ----------------------------------------------------------------

def test_console_credit_rules():
    s, p4, console, _ = ready()
    assert p4.open_console(1) and ops(drain(s)) == [(Op.OPEN_OK, 1, le32(4096))]
    assert p4.send(Op.DATA, 1, bytes(100), 0) and console.bytes[1] == bytes(100)
    assert drain(s, 99) == []                                              # 100 B, 99 ms: not yet
    assert ops(drain(s, 100)) == [(Op.CREDIT, 1, le32(4196))]              # 100 ms after the grant
    p4.now = 110
    assert p4.send(Op.DATA, 1, bytes(200), 0) and p4.send(Op.DATA, 1, bytes(200), 0)
    assert drain(s, 120) == []
    assert p4.send(Op.DATA, 1, bytes(112), 0)                              # 512 B past the grant
    assert ops(drain(s, 121)) == [(Op.CREDIT, 1, le32(4708))]


def test_credit_threshold_is_512_bytes():
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s)
    assert p4.send(Op.DATA, 1, bytes(207), 0) and p4.send(Op.DATA, 1, bytes(207), 0)
    assert p4.send(Op.DATA, 1, bytes(97), 0) and drain(s, 50) == []          # 511 B past the grant, 50 ms
    assert p4.send(Op.DATA, 1, b"x", 0) and ops(drain(s, 50)) == [(Op.CREDIT, 1, le32(4096 + 512))]


def test_relay_credit_and_reads():
    s, p4, _, relay = ready(answers={1: ACCEPT})
    assert p4.open_relay(1)
    assert ops(drain(s)) == [(Op.OPEN_OK, 1, le32(1024)), (Op.CREDIT, 1, le32(4096))]  # X1: consumed + 4096

    class Slow(ScriptedRelay):
        def relay_data(self, stream):
            pass  # reads later
    slow = Slow()
    s2 = Session(219, 0, relay=slow, relay_on=True, random=fixed_random())
    slow.session = s2
    p = P4(s2)
    assert p.hello() and drain(s2) and p.open_relay(1) and s2.relay_connected(1, 0) and drain(s2)
    assert all(p.send(Op.DATA, 1, bytes(200), 0) for _ in range(3)) and drain(s2, 200) == []  # unread: no credit
    assert len(s2.relay_read(1, 512)) == 512 and ops(drain(s2, 200)) == [(Op.CREDIT, 1, le32(4608))]
    assert s2.streams()[0].down_buffered == 88 and s2.relay_read(1) == bytes(88)


def test_flow_violations():
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s)                                 # OPEN_OK 4096 went out
    assert all(p4.send(Op.DATA, 1, bytes(207), 0) for _ in range(19)) and p4.send(Op.DATA, 1, bytes(163), 0)
    assert not p4.send(Op.DATA, 1, b"x", 0) and failed(s, Error.FLOW_VIOLATION)  # 4097 with no CREDIT sent
    s, p4, _, _ = ready(answers={1: ACCEPT})
    assert p4.open_relay(1) and drain(s)  # OPEN_OK 1024, CREDIT 4096 both sent
    for _ in range(19):
        assert p4.send(Op.DATA, 1, bytes(207), 0)
    assert not p4.send(Op.DATA, 1, bytes(207), 0) and failed(s, Error.FLOW_VIOLATION)  # 4140 > 4096
    s, p4, _, _ = ready(answers={1: ACCEPT})
    assert p4.open_relay(1) and s.peek(0) is not None                     # OPEN_OK peeked, not accepted
    assert not p4.send(Op.DATA, 1, b"x", 0) and failed(s)                  # DATA before OPEN_OK went out
    s, p4, _, _ = ready(answers={1: CONNECTING})
    assert p4.open_relay(1) and not p4.send(Op.CREDIT, 1, le32(1024)) and failed(s)  # CREDIT while connecting


def test_peer_credit_bounds():
    for limit, ok in ((1024, True), (4096, True), (1023, False), (4097, False)):
        s, p4, _, _ = ready(answers={1: ACCEPT})
        assert p4.open_relay(1) and drain(s)
        assert p4.send(Op.CREDIT, 1, le32(limit)) == ok and (failed(s, Error.FLOW_VIOLATION) != ok), limit


def test_data_out_round_robin_within_credit():
    s, p4, _, _ = ready(answers={1: ACCEPT, 3: ACCEPT})
    assert p4.open_relay(1) and p4.open_console(2) and p4.open_relay(3)
    drain(s)
    assert s.relay_send(1, bytes([1]) * 500) == 500 and s.console_send(bytes([2]) * 300)
    assert s.relay_send(3, bytes([3]) * 100) == 100
    out = [(f.stream, len(f.body)) for f in drain(s) if f.op == Op.DATA]
    assert out == [(1, 207), (2, 207), (3, 100), (1, 207), (2, 93), (1, 86)]
    assert s.relay_send(1, bytes(2000)) == 2000                            # only 1024 - 500 left in credit
    sent = sum(len(f.body) for f in drain(s) if f.op == Op.DATA)
    assert sent == 524 and s.streams()[0].sent == 1024
    assert p4.send(Op.CREDIT, 1, le32(1500)) and sum(len(f.body) for f in drain(s)) == 476


def test_console_send_is_all_or_nothing():
    s, p4, _, _ = ready()
    assert not s.console_send(b"x")                                         # no console
    assert p4.open_console(1)
    assert s.console_space() == 4096 and s.console_send(bytes(4000)) and not s.console_send(bytes(97))
    assert s.console_send(bytes(96)) and s.console_space() == 0 and not s.console_send(b"")


def test_relay_controls_ignore_the_console():
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s)
    assert s.relay_space(1) == 0 and s.relay_send(1, b"x") == 0 and s.relay_read(1) == b""
    s.relay_eof(1)
    assert not s.relay_fail(1, 7) and s.tick(5) and drain(s, 5) == []


# ---- CLOSE, RESET, FIN --------------------------------------------------------------------

def test_peer_close_bounds_by_class():
    for opener, reason, ok in (("relay", 1, True), ("relay", 2, False), ("console", 0, True), ("console", 5, True),
                               ("console", 6, False)):
        s, p4, console, relay = ready(answers={1: ACCEPT})
        assert (p4.open_relay(1) if opener == "relay" else p4.open_console(1)) and drain(s)
        assert p4.send(Op.CLOSE, 1, le16(reason)) == ok, (opener, reason)
        if ok:
            assert ops(drain(s)) == [(Op.CLOSE_ACK, 1, b"")] and s.streams() == []
            ends = relay.ends if opener == "relay" else console.ends
            assert ends == [StreamEnd(1, EndHow.PEER_CLOSE, reason)]
        else:
            assert failed(s)


def test_close_on_refused_and_reset_ids():
    """§6.2 #2: CLOSE 0..5 on an id the phone refused or reset gets CLOSE_ACK; 6 is still BAD."""
    s, p4, _, _ = ready(console_wanted=False, answers={2: Error.CONNECT_FAILED})
    assert p4.open_console(1) and p4.open_relay(2) and drain(s)
    assert p4.send(Op.CLOSE, 1, le16(4)) and p4.send(Op.CLOSE, 2, le16(5))   # relay id, reason 5: not live
    assert ops(drain(s)) == [(Op.CLOSE_ACK, 1, b""), (Op.CLOSE_ACK, 2, b"")]
    assert not p4.send(Op.CLOSE, 2, le16(6)) and failed(s)
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s) and s.console_reset(1)
    assert ops(drain(s)) == [(Op.RESET, 1, le16(1))]
    assert p4.send(Op.DATA, 1, b"late", 0) and p4.send(Op.CREDIT, 1, le32(5000))  # late frames dropped
    assert p4.send(Op.CLOSE, 1, le16(2)) and ops(drain(s)) == [(Op.CLOSE_ACK, 1, b"")]
    assert not p4.send(Op.CLOSE, 7, le16(0)) and failed(s)  # an id never opened


def test_peer_close_purges_unstarted_messages():
    s, p4, _, relay = ready(answers={1: ACCEPT})
    assert p4.open_relay(1)                                                    # OPEN_OK queued, not sent
    assert p4.send(Op.CLOSE, 1, le16(1)) and ops(drain(s)) == [(Op.CLOSE_ACK, 1, b"")]
    assert relay.ends == [StreamEnd(1, EndHow.PEER_CLOSE, 1)]
    s, p4, _, relay = ready()
    assert p4.open_relay(1) and p4.send(Op.CLOSE, 1, le16(1))                 # a connect cancelled
    assert ops(drain(s)) == [(Op.CLOSE_ACK, 1, b"")] and not s.relay_connected(1, 1)


def test_peer_stream_reset():
    s, p4, console, _ = ready()
    assert p4.open_console(1) and drain(s) and p4.send(Op.RESET, 1, le16(Error.DEADLINE))
    assert console.ends == [StreamEnd(1, EndHow.PEER_RESET, Error.DEADLINE)] and drain(s) == []
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s) and not p4.send(Op.RESET, 1, le16(19)) and failed(s)


def test_session_reset_from_the_p4():
    s, p4, console, relay = ready(answers={2: CONNECTING})
    assert p4.open_console(1) and p4.open_relay(2)
    assert not p4.send(Op.RESET, 0, le16(Error.PAUSED))
    assert s.end == SessionEnd(EndKind.PEER_RESET, Error.PAUSED) and s.failure_frame() is None
    assert console.ends == [StreamEnd(1, EndHow.SESSION, 18)] and relay.ends == [StreamEnd(2, EndHow.SESSION, 18)]
    s, p4, _, _ = ready()
    assert not p4.send(Op.RESET, 0, le16(19)) and failed(s)


def test_phone_console_close():
    s, p4, console, _ = ready()
    assert p4.open_console(1) and drain(s)
    assert s.console_send(b"queued") and s.console_close(4) and not s.console_send(b"x")
    assert s.tick(1)
    assert ops(drain(s, 1)) == [(Op.CLOSE, 1, le16(4)), (Op.LINK_STATE, 0, le32(3))]  # §6.2 #3
    assert p4.send(Op.DATA, 1, b"crossed", 0) and p4.send(Op.CREDIT, 1, le32(5000))     # dropped while closing
    assert console.bytes[1] == b"" and console.ends == []
    assert p4.send(Op.CLOSE_ACK, 1) and console.ends == [StreamEnd(1, EndHow.LOCAL_CLOSE, 4)]
    assert p4.send(Op.CLOSE_ACK, 1)                                                     # late: dropped
    assert raises(lambda: s.console_close(6))


def test_close_ack_rules():
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s) and not p4.send(Op.CLOSE_ACK, 1) and failed(s)  # we sent no CLOSE
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s) and s.console_close() and drain(s, 10)
    assert s.tick(2009) and not s.tick(2010) and failed(s, Error.DEADLINE)                 # 2 s for CLOSE_ACK
    s, p4, console, _ = ready()
    assert p4.open_console(1) and drain(s) and s.console_close()
    assert p4.send(Op.CLOSE, 1, le16(2))                                     # crossed our unsent CLOSE
    assert ops(drain(s)) == [(Op.CLOSE_ACK, 1, b"")] and console.ends == [StreamEnd(1, EndHow.PEER_CLOSE, 2)]
    s, p4, console, _ = ready()
    assert p4.open_console(1) and drain(s) and s.console_close() and drain(s)  # our CLOSE went out
    assert p4.send(Op.CLOSE, 1, le16(2)) and ops(drain(s)) == [(Op.CLOSE_ACK, 1, b"")]
    assert p4.send(Op.CLOSE_ACK, 1) and console.ends == [StreamEnd(1, EndHow.PEER_CLOSE, 2)]


def test_fin_rules():
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s) and not p4.send(Op.FIN, 1, le32(0)) and failed(s)  # §5.1
    s, p4, _, relay = ready(answers={1: ACCEPT})
    assert p4.open_relay(1) and drain(s) and p4.send(Op.DATA, 1, b"abc", 0)
    assert p4.send(Op.FIN, 1, le32(3)) and relay.fins == [1] and relay.received[1] == b"abc"
    assert not p4.send(Op.DATA, 1, b"x", 0) and failed(s)                 # DATA after FIN
    for count in (2, 4):
        s, p4, _, _ = ready(answers={1: ACCEPT})
        assert p4.open_relay(1) and drain(s) and p4.send(Op.DATA, 1, b"abc", 0)
        assert not p4.send(Op.FIN, 1, le32(count)) and failed(s), count
    s, p4, _, _ = ready(answers={1: ACCEPT})
    assert p4.open_relay(1) and drain(s) and p4.send(Op.FIN, 1, le32(0)) and not p4.send(Op.FIN, 1, le32(0))


def test_phone_fin_after_drain():
    s, p4, _, _ = ready(answers={1: ACCEPT})
    assert p4.open_relay(1) and drain(s)
    assert s.relay_send(1, bytes(300)) == 300
    s.relay_eof(1)
    assert s.relay_space(1) == 0 and s.relay_send(1, b"more") == 0
    out = []
    for t in range(3):
        s.tick(t)
        out += drain(s, t)
    assert [(f.op, len(f.body)) for f in out] == [(Op.DATA, 207), (Op.DATA, 93), (Op.FIN, 4)]
    assert out[-1].body == le32(300)


def test_local_stream_end_open_err_or_reset():
    s, p4, _, relay = ready(answers={1: ACCEPT})
    assert p4.open_relay(1) and s.relay_fail(1, Error.TCP_IO)                   # OPEN_OK not started
    assert ops(drain(s)) == [(Op.OPEN_ERR, 1, le16(Error.TCP_IO))]
    assert relay.ends == [StreamEnd(1, EndHow.REFUSED, Error.TCP_IO)]
    s, p4, _, relay = ready(answers={1: ACCEPT})
    assert p4.open_relay(1) and s.peek(0) is not None                        # OPEN_OK pending: started
    assert s.relay_fail(1, Error.TCP_IO) and s.accept(0)
    assert ops(drain(s)) == [(Op.RESET, 1, le16(Error.TCP_IO))] and s.stats.local_resets == {11: 1}
    assert raises(lambda: s.relay_fail(1, 0)) and raises(lambda: s.console_reset(19))


def test_malformed_console_record_resets_the_stream():
    s, p4, console, _ = ready()
    console.malformed_at = 4
    assert p4.open_console(1) and drain(s)
    assert p4.send(Op.DATA, 1, b"\x01\x02\x00\x00", 0)
    assert ops(drain(s)) == [(Op.RESET, 1, le16(1))] and console.ends == [StreamEnd(1, EndHow.LOCAL_RESET, 1)]
    assert p4.send(Op.DATA, 1, b"more", 0) and s.ready                        # the retired id: dropped


# ---- PING, PONG, silence --------------------------------------------------------------------

def test_pong_answers_and_coalesces():
    s, p4, _, _ = ready()
    assert p4.send(Op.PING, 0, le32(1)) and p4.send(Op.PING, 0, le32(2))
    assert ops(drain(s)) == [(Op.PONG, 0, le32(2))] and s.stats.pings_in == 2


def test_relay_ping_after_5s_silence_only_with_an_open_relay():
    s, p4, _, _ = ready(random=fixed_random(cookies=[b"abcd"]))
    assert p4.open_console(1) and drain(s)
    assert s.tick(10_000) and drain(s, 10_000) == []                         # the console alone: no PING
    s, p4, _, _ = ready(answers={1: ACCEPT}, random=fixed_random(cookies=[b"abcd"]))
    assert p4.open_relay(1) and drain(s)
    assert drain(s, 4999) == [] and ops(drain(s, 5000)) == [(Op.PING, 0, b"abcd")]
    assert s.ping_pending and drain(s, 9000) == []                           # one outstanding
    assert s.tick(9999) and not s.tick(10_000) and failed(s, Error.DEADLINE)  # PONG due in 5 s
    s, p4, _, _ = ready(answers={1: ACCEPT}, random=fixed_random(cookies=[b"abcd"]))
    assert p4.open_relay(1) and drain(s) and drain(s, 5000)
    assert not p4.send(Op.PONG, 0, b"abce") and failed(s)                    # wrong cookie


def test_local_ping_and_unsolicited_pong():
    s, p4, _, _ = ready(random=fixed_random(cookies=[b"wxyz"]))
    assert s.ping(0) and not s.ping(0) and ops(drain(s)) == [(Op.PING, 0, b"wxyz")]
    assert p4.send(Op.PONG, 0, b"wxyz") and not s.ping_pending and s.stats.pings_out == 1
    assert not p4.send(Op.PONG, 0, b"wxyz") and failed(s)                    # nothing outstanding


def test_silence_150s():
    s, p4, _, _ = ready()
    p4.now = 100_000
    assert p4.send(Op.PING, 0, le32(1))                                      # RX resets the clock
    assert s.tick(249_999) and not s.tick(250_000) and failed(s, Error.DEADLINE)
    assert s.failure_frame() == build_frame(Op.RESET, 0, 1, le16(Error.DEADLINE))


# ---- the control queue, fragments, peek/accept -----------------------------------------------

def test_control_queue_holds_12_and_fails_the_13th():
    s, p4, _, _ = ready(console_wanted=False)
    assert p4.open_console(1)                                                # OPEN_ERR 16 queued (1)
    for _ in range(11):
        assert p4.send(Op.CLOSE, 1, le16(0))                                 # 11 CLOSE_ACKs (12 in all)
    assert s.queued_control == 12 and s.ready
    assert not p4.send(Op.CLOSE, 1, le16(0)) and failed(s, Error.NO_RESOURCES)


def test_credit_coalesces_in_place():
    s, p4, _, _ = ready()
    assert p4.open_console(1) and drain(s)
    assert p4.send(Op.PING, 0, le32(9))                                      # its PONG holds the queue's head
    assert all(p4.send(Op.DATA, 1, bytes(207), 0) for _ in range(3))         # 621 B: a grant is due
    frame = s.peek(1)                                                        # PONG pending, CREDIT 4717 behind
    assert Frame.parse(frame).op == Op.PONG and s.queued_control == 2
    assert all(p4.send(Op.DATA, 1, bytes(207), 0) for _ in range(3)) and s.accept(1)
    assert ops(drain(s, 2)) == [(Op.CREDIT, 1, le32(4096 + 1242))]          # one CREDIT, the newest limit
    s2, p, _, _ = ready()
    assert p.open_console(1) and drain(s2)
    assert all(p.send(Op.DATA, 1, bytes(207), 0) for _ in range(3))
    assert Frame.parse(s2.peek(1)).body == le32(4717)                        # CREDIT pending: started
    assert all(p.send(Op.DATA, 1, bytes(207), 0) for _ in range(3)) and s2.accept(1)
    assert ops(drain(s2, 2)) == [(Op.CREDIT, 1, le32(5338))]                  # the next grant, not a repeat


def test_control_fragments_at_a_small_value():
    """At P = 20 the P4's 12-byte HELLO arrives in two fragments and the 20-byte HELLO_ACK leaves in
    three (8 + 8 + 4 body bytes)."""
    s, _, _ = session(max_frame=20)
    p4 = P4(s)
    body = P4_NONCE + bytes((3, 1)) + le16(20)
    assert not P4(session(max_frame=20)[0]).send(Op.HELLO, 0, body)         # 24 > 20: too long whole
    assert p4.send(Op.HELLO, 0, body[:8], inner.FIRST) and p4.send(Op.HELLO, 0, body[8:], inner.LAST)
    frames = drain(s)
    assert [(f.op, f.flags, len(f.body)) for f in frames] == [(Op.HELLO_ACK, inner.FIRST, 8), (Op.HELLO_ACK, 0, 8),
                                                              (Op.HELLO_ACK, inner.LAST, 4)]
    assert [f.seq for f in frames] == [0, 1, 2] and s.hello_ack_sent


def test_peek_returns_the_same_frame_until_accept():
    s, p4, _, _ = ready()
    assert p4.send(Op.PING, 0, le32(1))
    first = s.peek(0)
    assert s.peek(5) is first and s.tx_seq == 1 and s.accept(5) and s.tx_seq == 2
    assert s.peek(6) is None and not s.accept(6) and s.stats.frames_out == 2


def test_failure_frame_rules():
    s, p4, _, _ = ready()
    assert not p4.send(Op.PING, 0, le32(1), seq=9)
    assert s.failure_frame() == build_frame(Op.RESET, 0, 1, le16(1)) and s.peek(0) is None
    assert s.tick(1) is False and not s.receive(build_frame(Op.PING, 0, 1, le32(1)), 1)


def test_end_session_and_link_lost():
    s, p4, console, _ = ready()
    assert p4.open_console(1) and p4.send(Op.PING, 0, le32(3))
    assert s.end_session() and not s.end_session() and not s.ping(0)
    assert ops(drain(s)) == [(Op.OPEN_OK, 1, le32(4096)), (Op.PONG, 0, le32(3)), (Op.RESET, 0, le16(18))]
    assert s.end == SessionEnd(EndKind.LOCAL_RESET, 18) and console.ends == [StreamEnd(1, EndHow.SESSION, 18)]
    assert raises(lambda: s.end_session(19))
    s, p4, console, _ = ready()
    assert p4.open_console(1)
    s.link_lost()
    assert s.end == SessionEnd(EndKind.LINK_LOST, 0) and console.ends == [StreamEnd(1, EndHow.SESSION, 0)]
    assert s.peek(0) is None and s.failure_frame() is None


def test_status_snapshot():
    s, p4, _, _ = ready(answers={2: ACCEPT})
    assert p4.open_console(1) and p4.open_relay(2) and drain(s)
    info = {i.stream: i for i in s.streams()}
    assert info[1].kind is StreamKind.CONSOLE and info[2].kind is StreamKind.RELAY
    assert (info[1].peer_limit, info[1].receive_limit, info[2].receive_limit) == (4096, 4096, 4096)
    assert s.stats.opens == 2 and s.stats.frames_in == 3 and AddressType.LOCAL == 4
