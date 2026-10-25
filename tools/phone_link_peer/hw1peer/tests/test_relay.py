"""relay: TcpRelay against a bare inner Session and real localhost sockets.

Each test drives the session with plaintext P4 frames (no BLE, no crypto) and runs the
relay's poll on a millisecond test clock, sleeping a little so real sockets make progress.
Covered: connect, refusal (7), name lookup failure (6), the session's connect deadline (8),
bytes both ways beyond the first window, the P4's FIN -> SHUT_WR, TCP EOF -> FIN, the relay
switch going off (OPEN_ERR or RESET 16), detach, and logs without destinations.
"""

import socket
import subprocess
import sys
import textwrap
import threading
import time

from hw1peer.inner import (WHOLE, Error, Frame, Op, Session, StreamKind, build_frame, le16, le32)
from hw1peer.relay import TcpRelay
from hw1peer.tests.pipe_harness import fixed_random

P4_NONCE = bytes(range(1, 9))
LOCALHOST = bytes([127, 0, 0, 1])


class Rig:
    """A READY session with a TcpRelay, a plaintext P4 and a test clock."""

    def __init__(self, resolver=None) -> None:
        self.log: list[str] = []
        kwargs = {"resolver": resolver} if resolver else {}
        self.relay = TcpRelay(log=self.log.append, **kwargs)
        self.session = Session(219, 0, relay=self.relay, relay_on=True, random=fixed_random())
        self.relay.attach(self.session)
        self.now = 0
        self.seq = 0
        self.frames: list[Frame] = []
        self.send(Op.HELLO, 0, P4_NONCE + bytes((3, 1)) + le16(219))
        self.drain()
        assert self.session.ready

    def send(self, op: int, stream: int = 0, body: bytes = b"", flags: int = WHOLE) -> bool:
        ok = self.session.receive(build_frame(op, stream, self.seq, body, flags), self.now)
        self.seq += 1
        return ok

    def open(self, stream: int, port: int, kind: int = 1, host: bytes = LOCALHOST) -> None:
        assert self.send(Op.OPEN, stream, le32(1024) + le16(port) + bytes((kind, len(host))) + host)

    def drain(self) -> list[Frame]:
        out = []
        while (frame := self.session.peek(self.now)) is not None:
            assert self.session.accept(self.now)
            parsed = Frame.parse(frame)
            assert parsed is not None
            out.append(parsed)
        self.frames += out
        return out

    def run(self, ms: int, until=None) -> None:
        for _ in range(ms):
            self.now += 1
            self.session.tick(self.now)
            self.relay.poll(self.now)
            self.drain()
            time.sleep(0.0002)
            if until is not None and until():
                return
        assert until is None, "condition not reached"

    def close(self) -> None:
        self.relay.shutdown()

    def __enter__(self) -> "Rig":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()

    def ops(self, stream: int) -> list[tuple[int, bytes]]:
        return [(f.op, f.body) for f in self.frames if f.stream == stream]

    def data_to_p4(self, stream: int) -> bytes:
        return b"".join(f.body for f in self.frames if f.stream == stream and f.op == Op.DATA)


def closed_port() -> int:
    """A localhost port with nothing listening (bound, then released)."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class Server:
    """Accepts one connection; echoes until EOF; records what it saw."""

    def __init__(self, close_after: int | None = None) -> None:
        self.server = socket.create_server(("127.0.0.1", 0))
        self.port = self.server.getsockname()[1]
        self.received = bytearray()
        self.eof = threading.Event()
        self.close_after = close_after
        threading.Thread(target=self._run, daemon=True).start()

    def _run(self) -> None:
        conn, _ = self.server.accept()
        with conn:
            while data := conn.recv(4096):
                self.received += data
                conn.sendall(data)
                if self.close_after is not None and len(self.received) >= self.close_after:
                    return
            self.eof.set()

    def close(self) -> None:
        self.server.close()


def test_connect_ok_then_open_ok_1024():
    server = Server()
    try:
        with Rig() as rig:
            rig.open(1, server.port)
            assert rig.ops(1) == [], "nothing until the TCP connect completes"
            rig.run(1000, until=lambda: any(op == Op.OPEN_OK for op, _ in rig.ops(1)))
            assert (Op.OPEN_OK, le32(1024)) in rig.ops(1)
            assert rig.relay.stats.connected == 1 and rig.relay.streams()[0].state == "open"
    finally:
        server.close()


def test_connect_refused_is_open_err_7():
    with Rig() as rig:
        rig.open(1, closed_port())
        rig.run(1000, until=lambda: any(op == Op.OPEN_ERR for op, _ in rig.ops(1)))
        assert (Op.OPEN_ERR, le16(Error.CONNECT_FAILED)) in rig.ops(1)
        assert rig.relay.stats.failed == {7: 1} and rig.relay.streams() == []


def test_name_lookup_failure_is_open_err_6():
    def fails(host: str, port: int):
        raise socket.gaierror("no such name")
    for resolver in (fails, lambda host, port: []):
        with Rig(resolver) as rig:
            rig.open(1, 443, kind=3, host=b"relay.example.test")
            rig.run(1000, until=lambda: any(op == Op.OPEN_ERR for op, _ in rig.ops(1)))
            assert (Op.OPEN_ERR, le16(Error.DNS_FAILED)) in rig.ops(1)


def test_slow_lookup_hits_the_sessions_connect_deadline():
    release = threading.Event()

    def slow(host: str, port: int):
        release.wait(5)
        return [(socket.AF_INET, ("127.0.0.1", port))]
    with Rig(slow) as rig:
        rig.open(1, 443, kind=3, host=b"slow.example.test")
        for _ in range(2499):
            rig.now += 1
            rig.session.tick(rig.now)
            rig.relay.poll(rig.now)
            rig.drain()
        assert not any(op == Op.OPEN_ERR for op, _ in rig.ops(1))
        rig.run(5, until=lambda: any(op == Op.OPEN_ERR for op, _ in rig.ops(1)))
        assert (Op.OPEN_ERR, le16(Error.CONNECT_TIMEOUT)) in rig.ops(1)
        release.set()
        rig.run(50)
        assert rig.relay.streams() == []


def test_bytes_both_ways_fin_and_eof():
    server = Server()
    try:
        with Rig() as rig:
            rig.open(1, server.port)
            rig.run(1000, until=lambda: any(op == Op.OPEN_OK for op, _ in rig.ops(1)))
            payload = bytes(range(256)) * 4  # 1024 B: exactly the relay's first window
            for at in range(0, len(payload), 200):
                assert rig.send(Op.DATA, 1, payload[at:at + 200], 0)
            assert rig.send(Op.FIN, 1, le32(len(payload)))
            rig.run(3000, until=lambda: server.eof.is_set())
            assert bytes(server.received) == payload, "the P4's bytes reached TCP, then SHUT_WR"
            # The echo comes back; the server's close is TCP EOF, so the session sends FIN after the bytes.
            rig.run(3000, until=lambda: any(op == Op.FIN for op, _ in rig.ops(1)))
            assert rig.data_to_p4(1) == payload
            assert (Op.FIN, le32(len(payload))) in rig.ops(1)
            credits = [int.from_bytes(body, "little") for op, body in rig.ops(1) if op == Op.CREDIT]
            assert credits and credits[-1] >= len(payload), "reading counts as consumed: credit follows"
    finally:
        server.close()


def test_relay_off_ends_streams_with_16():
    server = Server()
    release = threading.Event()

    def slow(host: str, port: int):
        release.wait(5)
        return []
    try:
        with Rig(slow) as rig:
            rig.open(1, server.port)
            rig.run(1000, until=lambda: any(op == Op.OPEN_OK for op, _ in rig.ops(1)))
            rig.open(2, 443, kind=3, host=b"pending.example.test")
            assert rig.relay.end_all(Error.SERVICE_OFF) == 2
            rig.drain()
            assert (Op.RESET, le16(16)) in rig.ops(1), "an open stream gets RESET 16"
            assert (Op.OPEN_ERR, le16(16)) in rig.ops(2), "a connecting one gets OPEN_ERR 16"
            assert rig.relay.streams() == []
            release.set()
    finally:
        server.close()


def test_detach_closes_sockets_and_logs_hold_no_destination():
    server = Server()
    try:
        with Rig() as rig:
            rig.open(1, server.port)
            rig.run(1000, until=lambda: any(op == Op.OPEN_OK for op, _ in rig.ops(1)))
            rig.relay.detach()
            assert rig.relay.streams() == []
            assert [s.kind for s in rig.session.streams()] == [StreamKind.RELAY]
            text = "\n".join(rig.log)
            assert "127.0.0.1" not in text and str(server.port) not in text
            assert "relay: stream 1 OPEN to an IPv4 target" in text and "relay: stream 1 connected" in text
    finally:
        server.close()


HUNG_LOOKUP_EXIT = textwrap.dedent("""
    import time
    from hw1peer.inner import AddressType, RelayTarget
    from hw1peer.relay import TcpRelay

    relay = TcpRelay(resolver=lambda host, port: time.sleep(30) or [])
    target = RelayTarget.from_wire(AddressType.DNS, b"hung.example.test", 443)
    relay.relay_open(1, target, 0)        # the lookup starts and hangs
    time.sleep(0.2)
    relay.shutdown()
    print("exiting", flush=True)
""")


def test_a_hung_lookup_never_delays_the_exit():
    """getaddrinfo can block for tens of seconds: the lookup runs on a daemon thread, so the
    process exits at once after shutdown (a ThreadPoolExecutor worker is joined at exit)."""
    started = time.monotonic()
    result = subprocess.run([sys.executable, "-c", HUNG_LOOKUP_EXIT], capture_output=True, text=True, timeout=60)
    took = time.monotonic() - started
    assert result.returncode == 0 and "exiting" in result.stdout, result.stderr[-2000:]
    assert took < 10, f"exit took {took:.1f} s"


def test_lookups_run_on_daemon_threads_and_a_late_answer_is_ignored():
    release = threading.Event()
    names: list[str] = []

    def resolver(host: str, port: int):
        names.append(threading.current_thread().name)
        assert threading.current_thread().daemon
        release.wait(5)
        return [(socket.AF_INET, ("127.0.0.1", 9))]
    with Rig(resolver) as rig:
        rig.open(1, 443, kind=3, host=b"late.example.test")
        rig.run(10)
        assert names == ["relay-dns"]
        started = time.monotonic()
        rig.relay.shutdown()   # returns at once although the lookup is still running
        assert time.monotonic() - started < 0.5
        release.set()
        rig.run(50)
        assert rig.relay.streams() == []
