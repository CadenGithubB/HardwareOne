"""x1: inner protocol v2 against X1 v2 (phone_pipe_v2_sessions.json), byte for byte.

Each of the four sessions (ATT MTU 64, 244, 247, 517) is replayed from the
phone's side: the P4's values are opened with the lane-A channel and handed to
an :class:`~hw1peer.inner.Session` at their ``t_ms``; the session's timers run
every millisecond and its frames go out through a 30 ms :class:`TxGate`, sealed
by the same channel. Every phone value after the handshake must equal the
fixture's, in order, byte for byte (inner frame and sealed value).

Only the X1 script's own actions are injected (the fixture's ``phone_rules``:
"OPEN_ERR, FIN, LINK_STATE, RESET, COMMAND and BINARY are the script's"), each
derived from the fixture's phone frames, never from what the session sent:

* a relay OPEN_OK: the relay endpoint reports its connect at that frame's time;
* a relay OPEN_ERR 16: relay forwarding goes off just before that OPEN's last
  fragment is delivered, so the session refuses it by itself and announces
  the change with its own LINK_STATE;
* a LINK_STATE whose flags differ from the session's: the flags change then;
* console DATA: each COMMAND/BINARY record is handed to ``console_send`` at the
  time of the frame carrying its first byte;
* relay DATA: the stream's bytes go to ``relay_send`` at its first DATA frame;
* FIN: ``relay_eof``; the stream-0 RESET 18: ``end_session(18)``.

Everything else (HELLO_ACK, console OPEN_OK, CREDIT, PONG, CLOSE_ACK, the
LINK_STATE after refusing 16, DATA framing and order) comes from the session.

Timing: the X1 phone queued its scripted answers between two 1 ms steps, so a
scripted answer left at least 1 ms after the P4 frame that caused it. The
session answers a console OPEN, and refuses a relay OPEN with 16, inside
``receive``, so those go out in the same millisecond when the gate is open: the
console OPEN_OKs of streams 1, 5, 6, 7 and 9 (8's waited for the 30 ms gap),
the OPEN_ERR 16 and the LINK_STATE queued behind it leave 1 ms earlier than in
the fixture. Every other phone frame leaves at the fixture's exact
millisecond, and no byte changes; test_x1_send_times pins exactly that.
"""

from collections import defaultdict
from collections.abc import Callable
from dataclasses import dataclass, field

from hw1peer import inner
from hw1peer.inner import EndHow, EndKind, Op, Session
from hw1peer.tests import load_fixture, unhex
from hw1peer.tests.pipe_harness import X1, Link, make_link, parse_records

type Action = Callable[[Session, int], object]


@dataclass
class Script:
    """The X1 script's actions: ``pre`` before a P4 frame (by its index), ``post`` at a time."""

    pre: dict[int, list[Action]] = field(default_factory=lambda: defaultdict(list))
    post: dict[int, list[Action]] = field(default_factory=lambda: defaultdict(list))


@dataclass
class Replay:
    mtu: int
    link: Link
    expected: list[dict]  # the fixture's phone frames after the handshake


def _phone_frames(frames: list[dict]) -> list[dict]:
    return [f for f in frames[5:] if f["dir"] == "phone_to_p4"]


def _open_kinds(frames: list[dict]) -> tuple[dict[int, int], dict[int, int]]:
    """Per P4 OPEN: its address type and the index of its LAST fragment."""
    kinds, last = {}, {}
    for f in frames[5:]:
        if f["dir"] == "p4_to_phone" and f.get("op") == "OPEN":
            body = unhex(f["body_hex"])
            if f["flags"] & inner.FIRST:
                kinds[f["stream"]] = body[6]
            if f["flags"] & inner.LAST:
                last[f["stream"]] = f["i"]
    return kinds, last


def derive_script(frames: list[dict]) -> Script:
    """The scripted actions behind X1's phone frames (see the module docstring)."""
    script = Script()
    kinds, open_last = _open_kinds(frames)
    console = {s for s, k in kinds.items() if k == inner.AddressType.LOCAL}
    flags = inner.RELAY_ON | inner.CONSOLE_WANTED
    up: dict[int, bytearray] = defaultdict(bytearray)
    offsets: dict[int, list[tuple[int, int]]] = defaultdict(list)  # stream -> (offset, t) per DATA frame
    relay_first: dict[int, int] = {}
    for f in _phone_frames(frames):
        op, stream, t, body = f["op"], f["stream"], f["t_ms"], unhex(f["body_hex"])
        match op:
            case "OPEN_OK" if stream not in console:
                script.post[t].append(lambda s, now, i=stream: s.relay_connected(i, now))
            case "OPEN_ERR":
                assert stream not in console and int.from_bytes(body, "little") == inner.Error.SERVICE_OFF
                script.pre[open_last[stream]].append(lambda s, now: s.set_relay_on(False))
                flags &= ~inner.RELAY_ON
            case "LINK_STATE" if body[0] != flags:
                flags = body[0]
                script.post[t].append(lambda s, now, f=flags: (s.set_relay_on(bool(f & inner.RELAY_ON)),
                                                                s.set_console_wanted(bool(f & inner.CONSOLE_WANTED))))
            case "FIN":
                script.post[t].append(lambda s, now, i=stream: s.relay_eof(i))
            case "RESET":
                assert stream == 0
                script.post[t].append(lambda s, now, code=int.from_bytes(body, "little"): s.end_session(code))
            case "DATA":
                offsets[stream].append((len(up[stream]), t))
                up[stream] += body
                relay_first.setdefault(stream, t)
            case "HELLO_ACK" | "OPEN_OK" | "CREDIT" | "PONG" | "CLOSE_ACK" | "LINK_STATE":
                pass  # the session's own
            case _:
                raise AssertionError(f"unexpected phone {op} in X1 (frame {f['i']})")
    for stream, data in up.items():
        if stream in console:
            for _type, _flags, _payload, at in parse_records(bytes(data)):
                t = max(t for offset, t in offsets[stream] if offset <= at)
                end = at + 4 + len(_payload)
                script.post[t].append(lambda s, now, r=bytes(data[at:end]): s.console_send(r))
        else:
            script.post[relay_first[stream]].append(lambda s, now, i=stream, d=bytes(data): s.relay_send(i, d))
    return script


def replay(mtu: int) -> Replay:
    """Replay one X1 session through a fresh Session; returns what the phone sent."""
    session = next(s for s in load_fixture(X1)["sessions"] if s["att_mtu"] == mtu)
    frames = session["frames"]
    start = frames[4]["t_ms"]  # CONFIRM_ACK verified: the session (and its HELLO deadline) starts
    link = make_link(mtu, start)
    script = derive_script(frames)
    p4_at: dict[int, list[dict]] = defaultdict(list)
    for f in frames[5:]:
        if f["dir"] == "p4_to_phone":
            p4_at[f["t_ms"]].append(f)
    end = frames[-1]["t_ms"] + 200
    for t in range(start, end):
        for f in p4_at.get(t, ()):
            for action in script.pre.get(f["i"], ()):
                action(link.session, t)
            assert link.p4(unhex(f["value_hex"]), t), (mtu, f["i"], link.session.end)
        for action in script.post.get(t, ()):
            action(link.session, t)
        link.step(t)
        if link.session.done:
            break
    return Replay(mtu, link, _phone_frames(frames))


_REPLAYS: dict[int, Replay] = {}


def replayed(mtu: int) -> Replay:
    if mtu not in _REPLAYS:
        _REPLAYS[mtu] = replay(mtu)
    return _REPLAYS[mtu]


MTUS = (64, 244, 247, 517)


def _describe(frame: bytes) -> str:
    f = inner.Frame.parse(frame)
    if f is None:
        return frame.hex()
    return f"{Op(f.op).name} stream {f.stream} seq {f.seq} flags {f.flags} body {f.body[:12].hex()}"


# ---- byte for byte ------------------------------------------------------------------------

def test_x1_phone_frames_byte_for_byte():
    """Every phone value after the handshake equals the fixture's, in order, at all four MTUs."""
    total = 0
    for mtu in MTUS:
        r = replayed(mtu)
        sent, expected = r.link.sent, r.expected
        for k, (mine, theirs) in enumerate(zip(sent, expected)):
            assert mine.inner == unhex(theirs["inner_hex"]), (
                f"MTU {mtu}, phone frame {k} (fixture i={theirs['i']}, t={theirs['t_ms']}): sent "
                f"{_describe(mine.inner)} at {mine.t}, fixture {_describe(unhex(theirs['inner_hex']))}")
            assert mine.value == unhex(theirs["value_hex"]), (mtu, theirs["i"])
        assert len(sent) == len(expected), (mtu, len(sent), len(expected))
        total += len(sent)
    assert total == 149 + 55 + 55 + 49


def test_x1_send_times():
    """Every phone frame leaves at the fixture's millisecond, except the answers the session gives
    inside ``receive`` (see the module docstring), which leave exactly 1 ms earlier."""
    early = [("OPEN_OK", 1), ("OPEN_ERR", 4), ("LINK_STATE", 0), ("OPEN_OK", 5), ("OPEN_OK", 6),
             ("OPEN_OK", 7), ("OPEN_OK", 9)]
    for mtu in MTUS:
        r = replayed(mtu)
        drift = [(theirs["op"], theirs["stream"], mine.t - theirs["t_ms"]) for mine, theirs in zip(r.link.sent,
                                                                                                    r.expected)]
        assert [(op, stream) for op, stream, d in drift if d] == early, mtu
        assert all(d in (0, -1) for _, _, d in drift), mtu


def test_x1_phone_frame_kinds():
    """What the session produced by itself (not the script) covers every automatic frame kind."""
    for mtu in MTUS:
        ops = {inner.Frame.parse(s.inner).op for s in replayed(mtu).link.sent}  # type: ignore[union-attr]
        assert {Op.HELLO_ACK, Op.OPEN_OK, Op.OPEN_ERR, Op.CREDIT, Op.PONG, Op.CLOSE_ACK, Op.LINK_STATE, Op.DATA,
                Op.FIN, Op.RESET} <= ops, mtu


# ---- what the endpoints saw -----------------------------------------------------------------

def test_x1_console_records_delivered():
    """Each console stream's bytes, as the sink got them, are exactly the fixture's P4 records."""
    x1 = load_fixture(X1)
    for session in x1["sessions"]:
        mtu = session["att_mtu"]
        console = replayed(mtu).link.console
        expected: dict[int, list[tuple[int, int, bytes]]] = defaultdict(list)
        for record in session["console_records"]:
            if record["dir"] == "p4_to_phone":
                assert record["verdict"] in ("deliver", "skip", "ignore"), record
                expected[record["stream"]].append((record["code"], record["flags"], unhex(record["payload_hex"])))
        assert console.opens == x1["targets"]["console"]["streams"], mtu
        for stream in console.opens:
            got = [(t, fl, p) for t, fl, p, _ in parse_records(bytes(console.bytes[stream]))]
            assert got == expected[stream], (mtu, stream)
        assert expected.keys() <= set(console.opens)


def test_x1_stream_ends():
    """Console CLOSE reasons 3, 1, 2, 4, 5 reach the sink; the last console and the session end
    with our RESET 18; relay 2 closed normally, 3 cancelled, 4 refused at admission."""
    for mtu in MTUS:
        link = replayed(mtu).link
        ends = [(e.stream, e.how, e.code) for e in link.console.ends]
        assert ends == [(1, EndHow.PEER_CLOSE, 3), (5, EndHow.PEER_CLOSE, 1), (6, EndHow.PEER_CLOSE, 2),
                        (7, EndHow.PEER_CLOSE, 4), (8, EndHow.PEER_CLOSE, 5), (9, EndHow.SESSION, 18)], mtu
        assert [(e.stream, e.how, e.code) for e in link.relay.ends] == [(2, EndHow.PEER_CLOSE, 0),
                                                                        (3, EndHow.PEER_CLOSE, 1)], mtu
        assert [s for s, _ in link.relay.opens] == [2, 3], mtu  # stream 4 was refused before the endpoint
        assert link.session.stats.refused == {inner.Error.SERVICE_OFF: 1}, mtu


def test_x1_relay_targets_and_bytes():
    x1 = load_fixture(X1)
    t = x1["targets"]
    for mtu in MTUS:
        link = replayed(mtu).link
        (s2, a), (s3, b) = link.relay.opens
        assert (s2, a.kind, a.host_text(), a.port) == (t["relay"]["stream"], 1, t["relay"]["ipv4"], 443)
        assert (s3, b.kind, b.host_text(), b.port) == (t["relay_cancelled"]["stream"], 2, t["relay_cancelled"]["ipv6"],
                                                       443)
        assert "192.0.2" not in repr(a) and "2001" not in repr(b)  # the repr hides destinations
        assert len(link.relay.received[2]) == 1100 and len(link.relay.received[3]) == 40, mtu


def test_x1_session_end():
    """The phone's RESET 18 (PAUSED) ended every session; nothing failed on the way."""
    for mtu in MTUS:
        s = replayed(mtu).link.session
        assert s.done and s.end == inner.SessionEnd(EndKind.LOCAL_RESET, inner.Error.PAUSED), (mtu, s.end)
        assert s.failure_frame() is None
        assert s.stats.pings_out == 0 and s.stats.pings_in == 2, mtu  # the P4's relay and link PINGs
        assert s.device_nonce == unhex(load_fixture(X1)["pipe_nonces"]["p4_hex"])


def test_x1_flags_and_hello_ack():
    x1 = load_fixture(X1)
    for mtu in MTUS:
        session = next(s for s in x1["sessions"] if s["att_mtu"] == mtu)
        ack = inner.Frame.parse(replayed(mtu).link.sent[0].inner)
        assert ack is not None and ack.op == Op.HELLO_ACK
        assert ack.body[8:16] == unhex(x1["pipe_nonces"]["phone_hex"])
        assert ack.body[16] == x1["max_streams"] and ack.body[17] == x1["phone_flags"]
        assert int.from_bytes(ack.body[18:20], "little") == session["inner_capacity"] == inner.frame_limit(mtu)
