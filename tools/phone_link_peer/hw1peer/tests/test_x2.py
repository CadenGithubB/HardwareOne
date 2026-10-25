"""x2: inner protocol v2 against X2 v2 (phone_pipe_v2_negative.json), every vector.

X2 has two halves, and every one of its 97 vectors is checked here
(test_x2_every_vector_is_checked fails on a vector nobody handles):

* **P4 to phone** (18; ``contract`` and ``contract_synthetic``): the answer the
  phone owes. Each is replayed into a Session from X2's ``base`` point (the
  MTU-247 X1 session after frame 4, 5 or 6), opening P4 values with the lane-A
  channel, and the session must give exactly the owed answer: a refusal code,
  CLOSE_ACK on a refused or reset id, the version signal, or a BAD_PROTOCOL
  failure. Phone frames the vector itself records are matched byte for byte.
  The four outer-layer ones (tampered CONFIRM_ACK, REJECT, zero key, replayed
  DATA) end in the channel before any Session exists.
* **Phone to P4** (79; ``p4_production``): what the production P4 did with a
  phone frame. The phone-side rule is the converse: the session must build the
  legal frames byte for byte and never build the illegal ones. So each vector is
  either reproduced (``BYTES``: the session, driven by the matching local
  control, emits the vector's phone frames exactly, outer counter included) or
  shown impossible (``NEVER``: the session's own frame differs from the vector's
  in exactly the field the vector broke, or the control that would build it
  refuses), or is outer/GATT-only (``OUTER``: no inner frame, or a broken outer
  layer around an inner frame the session does build). Each OUTER vector still
  gets a peer-side check: the channel never builds the vector's bad HELLO,
  CONFIRM, REJECT or early DATA, and the app (Peer over the fake GATT, with X1's
  synthetic keys) answers HELLO_ACK at once, never retransmits, ends its own
  handshake only after the P4's 5 s cap, drops a vanished link cleanly, sends
  nothing to a subscription it would refuse, and never touches the service
  while a link runs.
"""

import io
import os
from collections.abc import Callable
from dataclasses import dataclass

from hw1peer import channel, inner, paircode
from hw1peer.app import HANDSHAKE_DEADLINE_MS, Credentials, Output, Peer, Phase
from hw1peer.channel import Data, Established, Failure, Invalid, PhoneChannel, Rejected, Reply
from hw1peer.inner import (EndHow, EndKind, Error, Op, Session, SessionEnd, State, build_frame, le16, le32)
from hw1peer.tests import load_fixture, unhex
from hw1peer.tests import test_channel as tc
from hw1peer.tests.fake_gatt import Bench, FakePeripheral
from hw1peer.tests.p4_model import GattPhase
from hw1peer.tests.pipe_harness import X1, X2, Link, fixed_random, make_link, pipe_base, x1_session

VECTORS: dict[str, dict] = {v["id"]: v for v in load_fixture(X2)["vectors"]}
_CHECKS: dict[str, tuple[str, Callable[[dict], None]]] = {}

COOKIE = bytes.fromhex("09080706")  # the X2 generator's PING cookie


def check(kind: str, *ids: str) -> Callable[[Callable[[dict], None]], Callable[[dict], None]]:
    """Register ``fn`` as the check of each vector id (``kind`` is its group)."""
    def register(fn: Callable[[dict], None]) -> Callable[[dict], None]:
        for vector_id in ids:
            assert vector_id in VECTORS, vector_id
            assert vector_id not in _CHECKS, vector_id
            _CHECKS[vector_id] = (kind, fn)
        return fn
    return register


def run_group(kind: str) -> int:
    ran = 0
    for vector_id, (group, fn) in _CHECKS.items():
        if group == kind:
            fn(VECTORS[vector_id])
            ran += 1
    return ran


# ---- helpers --------------------------------------------------------------------------------

def phone_frames(v: dict) -> list[dict]:
    return [f for f in v["frames"] if f["dir"] == "phone_to_p4"]


def p4_frames(v: dict) -> list[dict]:
    return [f for f in v["frames"] if f["dir"] == "p4_to_phone"]


def scenario(link: Link, v: dict, actions: dict[int, Callable[[Session, int], object]] | None = None,
             pre: dict[int, Callable[[Session, int], object]] | None = None, start: int = 91,
             settle: int = 400) -> Link:
    """Play a vector against ``link``: P4 frames are delivered at their ``t_ms`` (``pre`` runs
    just before one), ``actions`` run at the time of the phone frame they cause, and the session
    is stepped every millisecond in between and ``settle`` ms after."""
    actions, pre = actions or {}, pre or {}
    t = start
    for f in v["frames"]:
        while t < f["t_ms"]:
            link.step(t)
            t += 1
        if f["dir"] == "p4_to_phone":
            if f["i"] in pre:
                pre[f["i"]](link.session, t)
            link.p4(unhex(f["value_hex"]), t)
        elif f["i"] in actions:
            actions[f["i"]](link.session, t)
    link.run(t, t + settle)
    return link


def assert_sent(link: Link, expected: list[dict], vector_id: str) -> None:
    """The session's first frames equal the vector's phone frames, inner and sealed value."""
    assert len(link.sent) >= len(expected), (vector_id, len(link.sent), len(expected))
    for mine, theirs in zip(link.sent, expected):
        assert mine.inner == unhex(theirs["inner_hex"]), (vector_id, theirs["i"], mine.inner.hex())
        assert mine.value == unhex(theirs["value_hex"]), (vector_id, theirs["i"])


def frame_of(sent_inner: bytes) -> inner.Frame:
    f = inner.Frame.parse(sent_inner)
    assert f is not None
    return f


def differs_only_at(mine: bytes, theirs: bytes, positions: set[int]) -> bool:
    """Same length and the bytes differ exactly at ``positions`` (inner-frame offsets)."""
    return len(mine) == len(theirs) and {i for i, (a, b) in enumerate(zip(mine, theirs)) if a != b} == positions


def ended_failed(s: Session, error: int = Error.BAD_PROTOCOL) -> bool:
    return s.done and s.end == SessionEnd(EndKind.FAILED, error)


def hello_link(**options) -> Link:
    """The pipe_hello base for a phone answer: the P4's pipe HELLO (X1 frame 5) delivered."""
    link = pipe_base(through_hello_ack=False, **options)
    hello = x1_session(247)["frames"][5]
    assert link.p4(unhex(hello["value_hex"]), hello["t_ms"])
    return link


def my_ping(seq: int, cookie: bytes = COOKIE) -> bytes:
    return build_frame(Op.PING, 0, seq, cookie)


# ---- P4 to phone: the contract vectors --------------------------------------------------------

@check("contract", "console_open_while_unwanted")
def _console_open_while_unwanted(v: dict) -> None:
    """The phone's LINK_STATE 0 crossed the P4's console OPEN: OPEN_ERR 16, then CLOSE_ACK for the
    CLOSE 4 that crossed the refusal (an id the phone refused: reasons 0..5 are accepted)."""
    opened, state, close = v["frames"]
    link = pipe_base()
    link.session.set_console_wanted(False)                       # before the OPEN reaches the phone
    link.session.set_relay_on(False)                             # the vector's LINK_STATE says 0
    link.run(91, state["t_ms"] + 1)
    assert_sent(link, [state], v["id"])                          # LINK_STATE 0, counter 2, seq 1
    assert link.p4(unhex(opened["value_hex"]), state["t_ms"] + 1)  # the OPEN arrives after it
    assert link.p4(unhex(close["value_hex"]), close["t_ms"])
    link.run(close["t_ms"], close["t_ms"] + 1500)
    out = [frame_of(s.inner) for s in link.sent[1:]]
    assert [(f.op, f.stream, f.body) for f in out[:2]] == [(Op.OPEN_ERR, 1, le16(Error.SERVICE_OFF)),
                                                         (Op.CLOSE_ACK, 1, b"")], v["id"]
    # §6.2 #3: a LINK_STATE with the current flags follows the 16, at least 1 s after the last one
    # was queued (at 91, the first tick after the flags fell; the P4 measures from queue time too).
    assert (out[2].op, out[2].body) == (Op.LINK_STATE, le32(0))
    assert link.sent[3].t == 91 + inner.LINK_STATE_SPACING_MS and len(link.sent) == 4
    assert link.session.state is State.READY and link.console.opens == []


@check("contract", "console_close_2_after_reset_1")
def _console_close_2_after_reset_1(v: dict) -> None:
    """The phone reset the console (RESET 1) as the P4's CLOSE 2 crossed it: CLOSE_ACK."""
    link = scenario(pipe_base(), v, actions={2: lambda s, now: s.console_reset(Error.BAD_PROTOCOL)})
    assert_sent(link, phone_frames(v), v["id"])                  # OPEN_OK 4096, RESET 1
    ack = frame_of(link.sent[2].inner)
    assert (ack.op, ack.stream, ack.seq, ack.body) == (Op.CLOSE_ACK, 1, 3, b""), v["id"]
    assert link.sent[2].value == channel.seal_data(unhex(load_fixture(X1)["phone_to_p4_key_hex"]), channel.DIR_P2D,
                                                   4, link.sent[2].inner)
    assert [(e.stream, e.how, e.code) for e in link.console.ends] == [(1, EndHow.LOCAL_RESET, 1)]
    assert link.session.state is State.READY and len(link.sent) == 3


@check("contract", "relay_open_while_relay_off")
def _relay_open_while_relay_off(v: dict) -> None:
    """Relay forwarding off (or no destination): OPEN_ERR 16 and the session continues."""
    # Relay switched off just before the OPEN: the session refuses it by itself.
    link = scenario(pipe_base(), v, pre={0: lambda s, now: s.set_relay_on(False)})
    out = [frame_of(s.inner) for s in link.sent]
    assert [(f.op, f.stream, f.body) for f in out] == [(Op.OPEN_ERR, 1, le16(Error.SERVICE_OFF)),
                                                       (Op.LINK_STATE, 0, le32(inner.CONSOLE_WANTED))], v["id"]
    assert link.relay.opens == [] and link.session.state is State.READY
    # Relay on but no destination: the endpoint refuses with 16; the LINK_STATE still follows.
    link = scenario(pipe_base(answers={1: Error.SERVICE_OFF}), v)
    out = [frame_of(s.inner) for s in link.sent]
    assert [(f.op, f.stream, f.body) for f in out] == [(Op.OPEN_ERR, 1, le16(Error.SERVICE_OFF)),
                                                       (Op.LINK_STATE, 0, le32(3))], v["id"]
    assert link.session.state is State.READY


@check("contract", "tampered_confirm_ack", "zero_device_public_key", "p4_reject")
def _handshake_contracts(v: dict) -> None:
    """Outer handshake failures: the channel fails (AUTH) and no inner Session ever starts."""
    x1 = load_fixture(X1)
    frames = x1_session(247)["frames"]
    phone = PhoneChannel(unhex(x1["psk_hex"]), private_key=unhex(x1["phone_private_key_hex"]),
                         nonce=unhex(x1["phone_nonce_hex"]))
    assert phone.hello() == unhex(frames[1]["value_hex"])
    value = unhex(v["frames"][0]["value_hex"])
    if v["id"] != "zero_device_public_key":
        assert phone.receive(unhex(frames[2]["value_hex"])) == Reply(unhex(frames[3]["value_hex"]))
    rx = phone.receive(value)
    assert isinstance(rx, Rejected if v["id"] == "p4_reject" else Invalid), (v["id"], rx)
    assert not phone.established and phone.failure is Failure.AUTH


@check("contract", "replayed_p4_data")
def _replayed_p4_data(v: dict) -> None:
    """A replayed P4 DATA is refused by the channel before the session sees it."""
    link = pipe_base()
    rx = link.channel.receive(unhex(v["frames"][0]["value_hex"]))
    assert isinstance(rx, Invalid) and link.channel.failure is Failure.AUTH
    assert link.session.state is State.READY  # untouched: the GATT layer disconnects on the channel failure


# ---- P4 to phone: the contract_synthetic vectors --------------------------------------------

def synthetic(v: dict, **options) -> Link:
    """Deliver a synthetic vector's P4 frames from its base: pipe (after HELLO_ACK) or
    pipe_hello (the vector's frame takes the place of the P4's HELLO)."""
    if v["stage"] == "pipe_hello":
        link = make_link(247, x1_session(247)["frames"][4]["t_ms"], **options)
        return scenario(link, v, start=61)
    return scenario(pipe_base(**options), v)


@check("synthetic", "p4_hello_v1")
def _p4_hello_v1(v: dict) -> None:
    """A v1 pipe HELLO: the exact v1-header RESET 2 as the first inner frame, then the end (a
    strike). Its sealed value is X2's version_reset_v1_header phone value, byte for byte."""
    link = synthetic(v)
    assert [s.inner for s in link.sent] == [inner.VERSION_RESET]
    assert link.sent[0].value == unhex(VECTORS["version_reset_v1_header"]["frames"][0]["value_hex"])
    s = link.session
    assert s.done and s.end == SessionEnd(EndKind.VERSION, Error.UNSUPPORTED_VERSION) and s.end.strike
    assert not s.hello_ack_sent and s.failure_frame() is None


@check("synthetic", "p4_hello_bad_flag_bit")
def _p4_hello_bad_flag(v: dict) -> None:
    link = synthetic(v)
    assert link.sent == [] and ended_failed(link.session)
    assert link.session.failure_frame() is None  # no HELLO_ACK went out: nothing safe to send


@check("synthetic", "console_open_len_1", "console_close_reason_6_from_p4", "relay_close_reason_2_from_p4",
       "fin_on_console_from_p4", "link_state_bad_flag_bit_from_p4", "p4_reset_19")
def _synthetic_bad_protocol(v: dict) -> None:
    """BAD_PROTOCOL: the session fails; the GATT layer sends the stream-0 RESET 1 and disconnects."""
    link = synthetic(v)
    s = link.session
    assert ended_failed(s), (v["id"], s.end)
    reset = s.failure_frame()
    assert reset == build_frame(Op.RESET, 0, s.tx_seq, le16(Error.BAD_PROTOCOL)), v["id"]
    if v["id"] in ("console_close_reason_6_from_p4", "fin_on_console_from_p4"):
        assert [frame_of(x.inner).op for x in link.sent] == [Op.OPEN_OK]          # the production console OPEN
        assert [(e.stream, e.how) for e in link.console.ends] == [(1, EndHow.SESSION)]
    if v["id"] == "relay_close_reason_2_from_p4":
        assert [(e.stream, e.how) for e in link.relay.ends] == [(1, EndHow.SESSION)]  # still connecting


@check("synthetic", "console_open_port_2")
def _console_open_port_2(v: dict) -> None:
    link = synthetic(v)
    out = [frame_of(x.inner) for x in link.sent]
    assert [(f.op, f.stream, f.body) for f in out] == [(Op.OPEN_ERR, 1, le16(Error.UNSUPPORTED))]
    assert link.session.state is State.READY and link.console.opens == []


@check("synthetic", "second_console_open")
def _second_console_open(v: dict) -> None:
    link = synthetic(v)
    out = [frame_of(x.inner) for x in link.sent]
    assert [(f.op, f.stream, f.body) for f in out] == [(Op.OPEN_OK, 1, le32(4096)), (Op.OPEN_ERR, 2, le16(Error.BUSY))]
    assert link.session.state is State.READY and link.session.console_stream() == 1 and link.console.ends == []


@check("synthetic", "third_relay_open")
def _third_relay_open(v: dict) -> None:
    link = synthetic(v)
    out = [frame_of(x.inner) for x in link.sent]
    assert [(f.op, f.stream, f.body) for f in out] == [(Op.OPEN_ERR, 3, le16(Error.BUSY))]
    assert [i for i, _ in link.relay.opens] == [1, 2] and link.relay.ends == []
    assert [x.stream for x in link.session.streams()] == [1, 2] and link.session.state is State.READY


# ---- phone to P4: frames the session builds byte for byte -----------------------------------

@check("bytes", "version_reset_v1_header")
def _version_reset_v1(v: dict) -> None:
    link = hello_link(mismatch=True)
    link.run(90, 91)
    assert_sent(link, phone_frames(v), v["id"])
    assert link.session.end == SessionEnd(EndKind.VERSION, Error.UNSUPPORTED_VERSION) and link.session.done


@check("bytes", "hello_ack_relay_off")
def _hello_ack_relay_off(v: dict) -> None:
    link = hello_link(relay_on=False)
    link.run(90, 91)
    assert_sent(link, phone_frames(v), v["id"])
    assert link.session.state is State.READY and link.session.local_flags == inner.CONSOLE_WANTED


@check("bytes", "counter_gap_allowed", "counter_2_pow_63")
def _pings_at_counters(v: dict) -> None:
    """The phone's PING (cookie from its random source) at the vector's outer counters; the P4's
    PONG clears it."""
    link = pipe_base(random=fixed_random(cookies=[COOKIE] * 4))
    for f in v["frames"]:
        if f["dir"] == "phone_to_p4":
            link.channel.set_counters_for_test(f["counter"], link.channel.rx_counter)
            assert link.session.ping(f["t_ms"])
            link.run(f["t_ms"], f["t_ms"] + 1)
            assert link.sent[-1].inner == unhex(f["inner_hex"]), (v["id"], f["i"])
            assert link.sent[-1].value == unhex(f["value_hex"]), (v["id"], f["i"])
        else:
            assert link.session.ping_pending
            assert link.p4(unhex(f["value_hex"]), f["t_ms"])
            assert not link.session.ping_pending
    assert link.session.state is State.READY and link.session.stats.pings_out == len(phone_frames(v))


@check("bytes", "session_reset_paused", "session_reset_version_after_ready")
def _session_resets(v: dict) -> None:
    """end_session(code) with a legal code: the vector's stream-0 RESET exactly."""
    (frame,) = phone_frames(v)
    code = int.from_bytes(unhex(frame["body_hex"]), "little")
    link = pipe_base()
    assert link.session.end_session(code)
    link.run(91, 200)
    assert_sent(link, [frame], v["id"])
    assert link.session.done and link.session.end == SessionEnd(EndKind.LOCAL_RESET, code)


@check("bytes", "phone_reset_stream_0")
def _failure_reset_13(v: dict) -> None:
    """RESET 13 on stream 0 is the phone's failure frame after a DEADLINE failure (150 s silence)."""
    (frame,) = phone_frames(v)
    link = pipe_base()
    link.session.tick(90 + inner.SILENCE_MS)
    assert ended_failed(link.session, Error.DEADLINE)
    reset = link.session.failure_frame()
    assert reset == unhex(frame["inner_hex"]) and link.channel.seal(reset) == unhex(frame["value_hex"])


@check("bytes", "link_state_relay_off")
def _link_state_relay_off(v: dict) -> None:
    link = pipe_base()
    link.session.set_relay_on(False)
    link.run(141, 142)
    assert_sent(link, phone_frames(v), v["id"])


@check("bytes", "open_err_vpn", "late_close_for_ended_stream", "retired_close_reason_5", "retired_close_reason_6",
       "control_queue_12", "control_queue_12_then_13")
def _relay_refused_by_endpoint(v: dict) -> None:
    """The relay endpoint refuses (5: VPN, 7: connect failed): the OPEN_ERR exactly. The phone
    never sends CLOSE on an id it refused, so the vectors' later phone CLOSEs are not ours; the
    P4's CLOSE_ACKs for that retired id are dropped and the session continues."""
    phone = phone_frames(v)
    code = int.from_bytes(unhex(phone[0]["body_hex"]), "little")
    link = scenario(pipe_base(answers={1: code}), v)  # every P4 frame, CLOSE_ACKs included, is taken
    assert_sent(link, phone[:1], v["id"])
    assert len(link.sent) == 1 and link.session.state is State.READY
    assert link.session.stats.refused == {code: 1}
    assert [(e.stream, e.how, e.code) for e in link.relay.ends] == [(1, EndHow.REFUSED, code)]


@check("bytes", "open_err_service_off_relay")
def _open_err_16_relay(v: dict) -> None:
    link = scenario(pipe_base(), v, pre={0: lambda s, now: s.set_relay_on(False)})
    assert_sent(link, phone_frames(v), v["id"])


@check("bytes", "open_err_service_off_console")
def _open_err_16_console(v: dict) -> None:
    link = scenario(pipe_base(), v, pre={0: lambda s, now: s.set_console_wanted(False)})
    assert_sent(link, phone_frames(v), v["id"])
    assert (frame_of(link.sent[1].inner).op, link.session.state) == (Op.LINK_STATE, State.READY)


@check("bytes", "console_open_ok_4096", "console_stream_reset_1")
def _console_open_ok(v: dict) -> None:
    link = scenario(pipe_base(), v, actions={2: lambda s, now: s.console_reset(Error.BAD_PROTOCOL)})
    assert_sent(link, phone_frames(v), v["id"])
    assert len(link.sent) == len(phone_frames(v)) and link.session.state is State.READY


@check("bytes", "open_err_unsupported_console")
def _open_err_17(v: dict) -> None:
    """The OPEN_ERR 17 this vector records is, byte for byte, the session's answer to a console OPEN
    on port 2 (X2's synthetic console_open_port_2) at the same point of the session."""
    link = scenario(pipe_base(), VECTORS["console_open_port_2"])
    assert_sent(link, phone_frames(v), v["id"])
    # The production OPEN (port 1, console wanted) is accepted instead.
    link = scenario(pipe_base(), v)
    assert frame_of(link.sent[0].inner).op == Op.OPEN_OK


@check("bytes", "console_close_reason_0", "console_close_reason_5")
def _console_close(v: dict) -> None:
    """The phone's console CLOSE 0/5 exactly; the P4's CLOSE_ACK ends it; LINK_STATE follows."""
    close = phone_frames(v)[1]
    reason = int.from_bytes(unhex(close["body_hex"]), "little")
    link = scenario(pipe_base(), v, actions={2: lambda s, now: s.console_close(reason)})
    assert_sent(link, phone_frames(v), v["id"])
    assert [(e.stream, e.how, e.code) for e in link.console.ends] == [(1, EndHow.LOCAL_CLOSE, reason)]
    tail = [frame_of(x.inner) for x in link.sent[2:]]
    assert [(f.op, f.body) for f in tail] == [(Op.LINK_STATE, le32(3))] and link.session.state is State.READY


@check("bytes", "console_receive_window_4096")
def _console_4096(v: dict) -> None:
    """4096 console bytes (the P4's whole OPEN window) in 207-byte DATA frames, exactly."""
    data = b"".join(unhex(f["body_hex"]) for f in phone_frames(v) if f["op"] == "DATA")
    assert len(data) == 4096
    link = scenario(pipe_base(), v, actions={2: lambda s, now: s.console_send(data)})
    assert_sent(link, phone_frames(v), v["id"])
    (console,) = link.session.streams()
    assert len(link.sent) == len(phone_frames(v)) and console.sent == console.peer_limit == 4096


# ---- phone to P4: frames the session never builds --------------------------------------------

@check("never", "version_reset_v2_header", "version_reset_v1_code_3")
def _never_other_version_signals(v: dict) -> None:
    """The phone's version signal is always the v1-header RESET 2 (the P4 also takes the v2 header;
    code 3 is not a version signal)."""
    (frame,) = phone_frames(v)
    positions = {0} if v["id"] == "version_reset_v2_header" else {12}
    assert differs_only_at(inner.VERSION_RESET, unhex(frame["inner_hex"]), positions), v["id"]


@check("never", "hello_reset_code_13", "hello_unanswered_disconnect", "hello_unanswered_disconnect_late",
       "hello_unanswered_deadline", "link_state_before_ready")
def _never_silent_or_early(v: dict) -> None:
    """Before HELLO_ACK the phone sends nothing but HELLO_ACK (or the version signal): no stream-0
    RESET, no LINK_STATE; and it answers the HELLO in the same millisecond."""
    link = make_link(247, 60)
    s = link.session
    s.set_relay_on(False)
    assert not s.end_session(13) and not s.ping(70)
    link.run(61, 90)
    assert link.sent == []
    hello = x1_session(247)["frames"][5]
    assert link.p4(unhex(hello["value_hex"]), 90)
    link.run(90, 91)
    assert [frame_of(x.inner).op for x in link.sent] == [Op.HELLO_ACK] and link.sent[0].t == 90
    # A failure before HELLO_ACK (here the 3 s deadline) has no RESET to send.
    late = make_link(247, 60)
    late.run(61, 60 + inner.HELLO_DEADLINE_MS + 1)
    assert ended_failed(late.session, Error.DEADLINE) and late.session.failure_frame() is None and late.sent == []


@check("never", "hello_ack_v1_header", "hello_ack_max_streams_2", "hello_ack_bad_flag_bit")
def _never_bad_hello_ack(v: dict) -> None:
    """The vector's HELLO_ACK is the session's own with exactly one field broken."""
    (frame,) = phone_frames(v)
    link = hello_link()
    link.run(90, 91)
    mine = link.sent[0].inner
    field = {"hello_ack_v1_header": 0, "hello_ack_max_streams_2": 12 + 16, "hello_ack_bad_flag_bit": 12 + 17}
    assert differs_only_at(mine, unhex(frame["inner_hex"]), {field[v["id"]]}), v["id"]
    for relay_on in (False, True):  # every flag the phone can set is a defined bit
        for wanted in (False, True):
            other = hello_link(relay_on=relay_on, console_wanted=wanted)
            other.run(90, 91)
            assert not frame_of(other.sent[0].inner).body[17] & ~inner.PHONE_FLAG_MASK


@check("never", "inner_sequence_gap", "inner_version_1_after_ready", "inner_version_3",
       "version_reset_after_ready", "frame_for_future_stream")
def _never_bad_header(v: dict) -> None:
    """After HELLO_ACK the phone's frames are v2, at contiguous sequence numbers, for streams that
    exist; the version signal is never sent after READY (even with ``mismatch`` set)."""
    (frame,) = phone_frames(v)
    theirs = unhex(frame["inner_hex"])
    link = pipe_base(random=fixed_random(cookies=[COOKIE]))
    link.session.mismatch = True  # only the HELLO answer looks at it
    assert link.session.ping(141)
    link.run(141, 400)
    assert [x.inner for x in link.sent] == [my_ping(1)]  # nothing else, no frame for any stream
    if v["id"] == "inner_sequence_gap":
        assert differs_only_at(my_ping(1), theirs, {4})
    elif v["id"] in ("inner_version_1_after_ready", "inner_version_3"):
        assert differs_only_at(my_ping(1), theirs, {0})
    elif v["id"] == "version_reset_after_ready":
        mine = build_frame(Op.RESET, 0, 1, le16(Error.UNSUPPORTED_VERSION))  # end_session(2) after READY
        assert differs_only_at(mine, theirs, {0})
    else:
        assert frame_of(theirs).stream == 5 and link.session.streams() == []


@check("never", "session_reset_19", "open_err_19", "stream_reset_19", "console_close_reason_6")
def _never_unknown_codes(v: dict) -> None:
    """Codes outside 1..18 (and console CLOSE reasons above 5) are refused by the local controls."""
    link = scenario(pipe_base(), v) if p4_frames(v) else pipe_base()
    s = link.session
    for call in (lambda: s.end_session(19), lambda: s.relay_fail(1, 19), lambda: s.console_reset(19),
                 lambda: s.console_close(6)):
        try:
            call()
        except ValueError:
            continue
        raise AssertionError(f"{v['id']}: a bad code was accepted")
    answers = make_link(247, 60, answers={1: 19})
    answers.p4(unhex(x1_session(247)["frames"][5]["value_hex"]), 90)
    opened = [f for f in VECTORS["open_err_vpn"]["frames"] if f["dir"] == "p4_to_phone"][0]
    try:
        answers.p4(unhex(opened["value_hex"]), 142)
    except ValueError:
        pass
    else:
        raise AssertionError("a relay endpoint answering 19 was accepted")


@check("never", "link_state_bad_flag_bit", "link_state_upper_bytes", "link_state_nonzero_stream",
       "link_state_short_body")
def _never_bad_link_state(v: dict) -> None:
    """Every LINK_STATE the phone can send: stream 0, a 4-byte body, flag bits 0-1 only."""
    link = pipe_base()
    t = 91
    for flags in (2, 0, 1, 3, 0, 3):
        link.session.set_relay_on(bool(flags & 1))
        link.session.set_console_wanted(bool(flags & 2))
        link.run(t, t + 1100)
        t += 1100
    states = [frame_of(x.inner) for x in link.sent]
    assert [int.from_bytes(f.body, "little") for f in states] == [2, 0, 1, 3, 0, 3]
    assert all(f.op == Op.LINK_STATE and f.stream == 0 and len(f.body) == 4 and f.flags == inner.WHOLE
               for f in states)


@check("never", "open_ok_window_4097", "open_ok_window_4096", "credit_beyond_sent_plus_4096",
       "credit_repeat_tolerated", "receive_window_overflow", "relay_close_reason_2")
def _never_bad_relay_grants(v: dict) -> None:
    """A relay OPEN gets OPEN_OK 1024 and then CREDIT consumed + 4096 (X1's rule), never above, never
    repeated; DATA never exceeds the P4's window; the phone never CLOSEs a relay stream."""
    data = b"".join(unhex(f["body_hex"]) for f in phone_frames(v) if f["op"] == "DATA")
    link = scenario(pipe_base(answers={1: inner.ACCEPT}), v,
                    actions={2: lambda s, now: s.relay_send(1, data)} if data else None)
    out = [frame_of(x.inner) for x in link.sent]
    assert (out[0].op, out[0].body) == (Op.OPEN_OK, le32(inner.RELAY_LIMIT))
    assert (out[1].op, out[1].body) == (Op.CREDIT, le32(4096))   # consumed (0) + 4096
    assert not any(f.op == Op.CLOSE for f in out)
    credits = [int.from_bytes(f.body, "little") for f in out if f.op == Op.CREDIT]
    assert credits == sorted(set(credits))
    if data:
        sent = b"".join(f.body for f in out if f.op == Op.DATA)
        assert sent == data[:inner.RELAY_LIMIT]                   # the P4's OPEN window, not 1025
        assert [f.body for f in out if f.op == Op.DATA][:4] == [unhex(f["body_hex"]) for f in phone_frames(v)[1:5]]
    first = phone_frames(v)[0]
    if v["id"] == "open_ok_window_4097":
        assert first["body_hex"] == le32(4097).hex()
    assert link.session.state is State.READY


@check("never", "console_credit_overrun", "console_receive_overflow", "fin_on_console")
def _never_bad_console(v: dict) -> None:
    """The console's grant stays 4096 until bytes are consumed; console_send never exceeds the P4's
    window; there is no FIN on the console."""
    link = scenario(pipe_base(), v)
    s = link.session
    assert [frame_of(x.inner).op for x in link.sent] == [Op.OPEN_OK]  # no CREDIT without consumption
    assert not s.console_send(bytes(4097)) and s.console_send(bytes(4096)) and not s.console_send(b"x")
    s.relay_eof(1)                                                    # a console id: ignored
    link.run(800, 1800)
    out = [frame_of(x.inner) for x in link.sent[1:]]
    assert sum(len(f.body) for f in out if f.op == Op.DATA) == 4096 and not any(f.op == Op.FIN for f in out)


@check("never", "open_err_busy_console")
def _never_busy_first_console(v: dict) -> None:
    """9 refuses only a second console: the first one (this vector's OPEN) gets OPEN_OK, and the
    session's own 9 (second_console_open) differs from this frame in the stream id and sequence."""
    link = scenario(pipe_base(), v)
    assert [frame_of(x.inner).op for x in link.sent] == [Op.OPEN_OK]
    second = synthetic(VECTORS["second_console_open"])
    mine = second.sent[1].inner
    assert differs_only_at(mine, unhex(phone_frames(v)[0]["inner_hex"]), {2, 4})


@check("never", "ping_flood_coalesced_pong")
def _never_ping_flood(v: dict) -> None:
    """One PING outstanding at a time (its frame is the vector's first, exactly); a PONG whose cookie
    is not the outstanding one fails the session (the P4's coalesced PONG cannot hide ours)."""
    first, *_ = phone_frames(v)
    link = pipe_base(random=fixed_random(cookies=[unhex(first["body_hex"])]))
    assert link.session.ping(141) and not link.session.ping(142)
    link.run(141, 142)
    assert_sent(link, [first], v["id"])
    pong = p4_frames(v)[0]
    assert pong["body_hex"] != first["body_hex"]
    assert not link.p4(unhex(pong["value_hex"]), 713) and ended_failed(link.session)


# ---- outer and GATT-only vectors -------------------------------------------------------------

def x1_phone(psk: bytes | None = None, label: str = channel.LABEL, key: bytes | None = None,
             nonce: bytes | None = None) -> PhoneChannel:
    """The peer's channel with X1's synthetic key and nonce (X2's handshake vectors use them)."""
    x1 = load_fixture(X1)
    return PhoneChannel(psk if psk is not None else unhex(x1["psk_hex"]),
                        private_key=key if key is not None else unhex(x1["phone_private_key_hex"]),
                        nonce=nonce if nonce is not None else unhex(x1["phone_nonce_hex"]), label=label)


def values(v: dict) -> list[bytes]:
    return [unhex(f["value_hex"]) for f in v["frames"]]


@dataclass
class App:
    """The app (Peer over a FakePeripheral) with X1's synthetic draws, subscribed by c1."""

    peer: Peer
    fake: FakePeripheral
    sent: list[bytes]
    now: int = 0

    def run(self, until: int) -> None:
        while self.now < until:
            self.now += 1
            self.peer.tick()

    def run_until(self, condition: Callable[[], bool], limit: int) -> int:
        start = self.now
        while not condition():
            assert self.now - start < limit, "condition not reached"
            self.now += 1
            self.peer.tick()
        return self.now - start


def x1_app(mtu: int = 247, subscribe: bool = True, creds: bool = True) -> App:
    x1 = load_fixture(X1)
    draws = {32: unhex(x1["phone_private_key_hex"]), 16: unhex(x1["phone_nonce_hex"]),
             8: unhex(x1["pipe_nonces"]["phone_hex"])}
    fake = FakePeripheral()
    app = App(None, fake, [])  # type: ignore[arg-type]
    app.peer = Peer(fake, Output(None, stream=io.StringIO()), clock=lambda: app.now,
                    random=lambda n: draws.get(n) or bytes(range(1, n + 1)))
    app.peer.start()
    if creds:
        app.peer.creds = Credentials(bytearray(unhex(x1["psk_hex"])), is_code=True)
    app.peer.tick()
    fake.connect("c1", mtu, app.sent.append)
    if subscribe:
        fake.subscribe("c1")
    return app


@check("outer", "wrong_passphrase", "console_domain", "tampered_confirm")
def _outer_confirm_from_other_keys(v: dict) -> None:
    """Under the right secret the peer's CONFIRM differs from the vector's: that one comes only
    from another secret (WrongPipe!456), the console's derivation (PBKDF2 salt and HKDF label
    HW1-SC-v1, which the app never uses) or one flipped tag bit. The P4's REJECT then fails the
    channel for good."""
    x1 = load_fixture(X1)
    hello, ack, theirs, reject = values(v)[1:5]
    mine = x1_phone()
    assert mine.hello() == hello
    good = mine.receive(ack)
    assert isinstance(good, Reply) and len(good.value) == len(theirs) == channel.CONFIRM_SIZE
    assert good.value != theirs and good.value == unhex(x1["sessions"][0]["frames"][3]["value_hex"])
    match v["id"]:
        case "wrong_passphrase":
            other = x1_phone(psk=paircode.psk(b"WrongPipe!456"))
        case "console_domain":
            other = x1_phone(psk=paircode.pbkdf2_sha256(x1["passphrase_utf8"].encode("utf-8"), b"HW1-SC-v1"),
                             label="HW1-SC-v1")
            assert x1_phone(psk=other._psk).hello() == hello  # the label alone changes the keys
        case _:
            other = None
            assert differs_only_at(good.value, theirs, {channel.CONFIRM_SIZE - 1})
            assert good.value[-1] ^ theirs[-1] == 1
    if other is not None:
        assert other.hello() == hello and other.receive(ack) == Reply(theirs)
    app = x1_app()
    assert app.peer.link is not None and app.peer.link.channel._label == channel.LABEL == "HW1-PIPE-v1"
    assert mine.receive(reject) == Rejected(2)
    assert mine.state is channel.State.FAILED and mine.failure is Failure.AUTH and mine.seal(b"x") is None


@check("outer", "zero_public_key", "low_order_public_key", "short_hello")
def _outer_bad_hello(v: dict) -> None:
    """hello() is always 49 B: 01 || X25519(drawn key, base point) || nonce. A clamped scalar is a
    nonzero multiple of 8 below 2^255 and the base point has prime order, so the key is never
    zero or of small order, whatever is drawn (zeros and 0xFF included); hello() runs once."""
    x1 = load_fixture(X1)
    theirs = values(v)[1]
    nonce = unhex(x1["phone_nonce_hex"])
    small = {unhex(point) for point in tc.LOW_ORDER_POINTS}
    for drawn in (bytes(32), b"\xff" * 32, unhex(x1["phone_private_key_hex"]), *(os.urandom(32) for _ in range(20))):
        phone = x1_phone(key=drawn, nonce=nonce)
        hello = phone.hello()
        assert len(hello) == channel.HELLO_SIZE and hello[0] == channel.MsgType.HELLO
        assert hello[1:33] == channel.public_key(drawn) and hello[33:] == nonce
        assert any(hello[1:33]) and hello[1:33] not in small
        try:
            phone.hello()
        except RuntimeError:
            pass
        else:
            raise AssertionError("a second HELLO")
    mine = x1_phone().hello()
    if v["id"] == "short_hello":
        assert len(theirs) == channel.HELLO_SIZE - 1 and theirs == mine[:-1]
    else:
        assert theirs[1:33] in small and differs_only_at(mine, theirs, {i for i in range(1, 33) if mine[i] != theirs[i]})
        assert mine[1:33] != theirs[1:33] and mine[33:] == theirs[33:]


@check("outer", "phone_sends_reject")
def _outer_no_phone_reject(v: dict) -> None:
    """No PhoneChannel path emits a 05 value: every value from every outcome (success, REJECT,
    a bad HELLO_ACK or CONFIRM_ACK, a tampered DATA, exhaustion) starts with 01, 03 or 10."""
    assert values(v)[1] == b"\x05\x02"
    golden = load_fixture(tc.GOLDEN_V2)
    ack, confirm_ack = unhex(golden["hello_ack_hex"]), unhex(golden["confirm_ack_hex"])

    def drive(hello_ack: bytes = ack, final: bytes = confirm_ack) -> list[bytes]:
        phone = tc.phone_for(golden)
        sent = [phone.hello()]
        if isinstance(reply := phone.receive(hello_ack), Reply):
            sent.append(reply.value)
        if phone.state is not channel.State.FAILED:
            phone.receive(final)
        sent += [x for x in (phone.seal(b"ping") for _ in range(3)) if x is not None]
        phone.receive(bytes([channel.MsgType.DATA]) + bytes(40))   # a bad DATA: it fails, says nothing
        sent += [x for x in (phone.seal(b"after"),) if x is not None]
        return sent
    out = (drive() + drive(hello_ack=b"\x05\x02") + drive(hello_ack=ack[:-1]) + drive(hello_ack=b"\x04" + ack[1:])
           + drive(final=b"\x05\x02") + drive(final=confirm_ack[:-1] + bytes([confirm_ack[-1] ^ 1])))
    phone = tc.established(golden)
    phone.set_counters_for_test(channel.U64_MAX - 1, 0)
    out.append(phone.seal(b"last"))
    assert phone.seal(b"none") is None and phone.failure is Failure.EXHAUSTED
    assert len(out) == 13 and {value[0] for value in out} == {channel.MsgType.HELLO, channel.MsgType.CONFIRM,
                                                              channel.MsgType.DATA}


@check("outer", "data_before_confirm")
def _outer_no_early_data(v: dict) -> None:
    """seal() is None until ESTABLISHED, and the app's next value after HELLO_ACK is its CONFIRM:
    no DATA before CONFIRM_ACK."""
    theirs = values(v)[3]
    phone = x1_phone()
    assert phone.seal(b"x") is None
    phone.hello()
    assert phone.seal(b"x") is None
    assert isinstance(phone.receive(values(v)[2]), Reply) and phone.seal(b"x") is None
    app = x1_app()
    app.fake.write("c1", values(v)[2])
    app.run(200)
    assert [value[0] for value in app.sent] == [channel.MsgType.HELLO, channel.MsgType.CONFIRM]
    assert theirs[0] == channel.MsgType.DATA and app.sent[1] != theirs


@check("outer", "peer_silent_after_hello_ack")
def _outer_never_silent(v: dict) -> None:
    """The app answers HELLO_ACK within one notification gap (30 ms) plus a tick: far inside the
    P4's 5 s cap from its CCCD write."""
    x1 = load_fixture(X1)
    app = x1_app()
    app.run(v["frames"][2]["t_ms"])
    app.fake.write("c1", values(v)[2])
    took = app.run_until(lambda: len(app.sent) == 2, 100)
    assert took <= inner.NOTIFY_GAP_MS + 10
    assert app.sent == [values(v)[1], unhex(x1["sessions"][0]["frames"][3]["value_hex"])]


@check("outer", "secure_expiry_local_stall")
def _outer_waits_for_the_p4(v: dict) -> None:
    """With no HELLO_ACK the app sends nothing more (no retransmit) and ends its handshake only
    after the P4's own 5 s cap, so the P4 decides first (A-5: no cooldown when it owed the step)."""
    assert HANDSHAKE_DEADLINE_MS > 5_000
    app = x1_app()
    app.run(HANDSHAKE_DEADLINE_MS - 1)
    assert app.sent == [values(v)[1]] and app.peer.link.phase is Phase.HANDSHAKE
    app.run(HANDSHAKE_DEADLINE_MS + 10)
    assert app.peer.link.phase is Phase.ENDED and app.sent == [values(v)[1]]


@check("outer", "hello_ack_completion_timeout", "disconnect_during_handshake")
def _outer_link_drops_mid_handshake(v: dict) -> None:
    """The app needs no P4 write completion: it answers the HELLO_ACK value at once. When the P4
    then tears the link down, the link ends at once, its keys are wiped, nothing more is sent,
    the service is left alone and advertising resumes."""
    app = x1_app()
    app.fake.write("c1", values(v)[2])
    app.run_until(lambda: len(app.sent) == 2, 100)
    link = app.peer.link
    app.fake.disconnect("c1")
    assert app.peer.link is None and link.phase is Phase.ENDED and "unsubscribed" in link.why
    assert link.channel.state is channel.State.FAILED
    app.run(app.now + 7_000)
    assert len(app.sent) == 2 and app.fake.service_resets == 0 and app.fake.advertising


@check("outer", "cccd_refused_0x11")
def _outer_refused_subscription(v: dict) -> None:
    """macOS cannot answer a CCCD write with 0x11: the app refuses by silence instead. A
    subscription without a secret, or a second central while a link runs, gets no value at all."""
    assert [f["kind"] for f in v["frames"]] == ["cccd_enable"]
    app = x1_app(creds=False)
    app.run(1_000)
    assert app.sent == [] and app.peer.link is None and app.peer.ignored_subscriptions == 1
    app = x1_app()
    other: list[bytes] = []
    app.fake.connect("c7", 247, other.append)
    app.fake.subscribe("c7")
    app.run(1_000)
    assert other == [] and app.peer.link.central == "c1" and app.peer.ignored_subscriptions == 1


@check("outer", "service_changed_after_ready")
def _outer_service_untouched_while_ready(v: dict) -> None:
    """The service is never removed or re-added while a READY link runs, whatever the user does
    (and a normal end does not touch it either): the P4 never sees Service Changed from us."""
    assert v["p4_error"] == "Protocol" and v["stage"] == "pipe"
    bench = Bench(settings=None)
    bench.console_up()
    for line in ("status", "stats", "cmd status", "relay on", "relay off", "console off", "console on",
                 "adv off", "adv on", "ping", "mismatch on", "mismatch off"):
        bench.command(line)
        bench.step(1_200)
    assert bench.p4.ready and bench.p4.ends == []
    bench.command("end")
    bench.until(lambda: bench.p4.phase is GattPhase.STOPPED, what="RESET 18")
    bench.step(7_000)
    assert bench.fake.service_resets == 0 and bench.p4.service_changes == 0


@check("outer", "mtu_63")
def _mtu_63(v: dict) -> None:
    """Below ATT MTU 64 there is no inner frame limit: no session can be built."""
    assert inner.frame_limit(63) == 0 and inner.frame_limit(64) == 36 and inner.frame_limit(517) == 487
    try:
        Session(inner.frame_limit(63), 0)
    except ValueError:
        return
    raise AssertionError("a session at MTU 63")


@check("outer", "counter_regression_after_gap", "replayed_counter", "counter_max_once", "tampered_data_tag",
       "plaintext_after_handshake", "secure_hello_after_ready", "truncated_data", "oversize_value")
def _outer_around_a_ping(v: dict) -> None:
    """A broken outer layer (counter, tag, size, plaintext, HELLO) around inner frames the session
    does build: every inner frame recorded is the session's own PING at that sequence."""
    for f in phone_frames(v):
        if "inner_hex" in f:
            assert unhex(f["inner_hex"]) == my_ping(f["seq"]), (v["id"], f["i"])
        elif f["kind"] == "plaintext_inner":
            assert unhex(f["value_hex"]) == my_ping(1), v["id"]
    if v["id"] == "oversize_value":  # the session never frames more than the negotiated value
        link = pipe_base()
        opened = p4_frames(VECTORS["console_open_ok_4096"])[0]
        link.p4(unhex(opened["value_hex"]), 142)
        assert link.session.console_send(bytes(4096))
        link.run(142, 1200)
        assert max(len(x.value) for x in link.sent) == inner.value_limit(247) == 244


# ---- tests ------------------------------------------------------------------------------------

def test_x2_contract_vectors():
    assert run_group("contract") == 7


def test_x2_contract_synthetic_vectors():
    assert run_group("synthetic") == 11


def test_x2_phone_frames_built_byte_for_byte():
    assert run_group("bytes") == 22


def test_x2_phone_frames_never_built():
    assert run_group("never") == 34


def test_x2_outer_vectors():
    assert run_group("outer") == 23


def test_x2_every_vector_is_checked():
    vectors = load_fixture(X2)["vectors"]
    assert len(vectors) == 97 and set(VECTORS) == set(_CHECKS)
    by_source = {}
    for v in vectors:
        by_source.setdefault(v["source"], []).append(_CHECKS[v["id"]][0])
    assert set(by_source["contract"]) == {"contract"} and set(by_source["contract_synthetic"]) == {"synthetic"}
    assert set(by_source["p4_production"]) == {"bytes", "never", "outer"}


def test_x2_channel_keys_match_x1():
    """X2 shares X1's keys: the phone's sealed values here use the X1 phone-to-P4 key."""
    x1 = load_fixture(X1)
    frame = phone_frames(VECTORS["session_reset_paused"])[0]
    assert channel.seal_data(unhex(x1["phone_to_p4_key_hex"]), channel.DIR_P2D, frame["counter"],
                             unhex(frame["inner_hex"])) == unhex(frame["value_hex"])
    assert isinstance(channel.open_data(unhex(x1["p4_to_phone_key_hex"]), channel.DIR_D2P,
                                        unhex(p4_frames(VECTORS["open_err_vpn"])[0]["value_hex"])), tuple)
