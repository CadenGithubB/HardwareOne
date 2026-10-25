"""An optional real TCP relay for the P4's relay streams (slots 0-1).

:class:`TcpRelay` is the :class:`hw1peer.inner.RelayEndpoint` the app attaches to every
session. While the user's relay switch is on (``relay on``, the phone's RELAY_ON bit) the
session admits relay OPENs and hands them here; while it is off the session refuses them
with 16 SERVICE_OFF itself and :meth:`TcpRelay.end_all` ends the live ones.

For each admitted OPEN the relay does what the Android app's connector does, minus the VPN:
one resolution (DNS names only, on a daemon thread because ``getaddrinfo`` blocks; a daemon
thread never holds up the tool's exit, even mid-lookup), the first address, one non-blocking
connect, no retries. Outcomes map to the pipe's errors:
6 DNS_FAILED, 7 CONNECT_FAILED, 11 TCP_IO; the session itself sends 8 CONNECT_TIMEOUT after
2.5 s. Then bytes flow both ways:

* P4 -> TCP: :meth:`Session.relay_read` (which counts as consumed, so credit follows) into a
  small per-stream buffer that a non-blocking ``send`` drains; it reads again only when the
  buffer is empty, so at most one window waits here. After the P4's FIN has drained, the
  socket's write side is shut (``SHUT_WR``).
* TCP -> P4: ``recv`` up to :meth:`Session.relay_space`, then :meth:`Session.relay_send`;
  EOF becomes :meth:`Session.relay_eof` (the session sends FIN once its buffer drained).

Everything runs on the app's (main) thread from :meth:`TcpRelay.poll`, called every tick;
the session's callbacks only mark work. Logs name streams and address TYPES only: no host,
address or port ever reaches the log.
"""

import errno
import select
import socket
import threading
from collections.abc import Callable
from concurrent.futures import Future
from dataclasses import dataclass, field
from enum import Enum
from typing import Final

from hw1peer.inner import (ACCEPT, CONNECTING, WINDOW, AddressType, EndHow, Error, RelayTarget, Session, StreamEnd)

#: One resolved address: (family, sockaddr) as socket.getaddrinfo gives them.
type Address = tuple[int, tuple]
#: Resolves a DNS name and port to addresses (blocking; runs on a worker thread).
type Resolver = Callable[[str, int], list[Address]]

RECV_CHUNK: Final = WINDOW
#: connect_ex results that mean "started" for a non-blocking socket.
_IN_PROGRESS: Final = frozenset((0, errno.EINPROGRESS, errno.EWOULDBLOCK, errno.EAGAIN))
_KIND_TEXT: Final = {AddressType.IPV4: "IPv4", AddressType.IPV6: "IPv6", AddressType.DNS: "DNS name"}


def system_resolver(host: str, port: int) -> list[Address]:
    """``getaddrinfo`` for a TCP stream: every answer, in the system's order."""
    return [(family, sockaddr) for family, _, _, _, sockaddr in
            socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)]


class ConnState(Enum):
    RESOLVING = "resolving"
    CONNECTING = "connecting"
    OPEN = "open"


@dataclass(eq=False)
class _Conn:
    stream: int
    target: RelayTarget = field(repr=False)
    state: ConnState
    sock: socket.socket | None = None
    lookup: Future | None = None
    out: bytearray = field(default_factory=bytearray)   # P4 bytes read from the session, not yet sent
    eof_in: bool = False                                  # the socket's EOF reached the session
    shut_wr: bool = False
    bytes_up: int = 0                                     # TCP -> P4
    bytes_down: int = 0                                   # P4 -> TCP


@dataclass(slots=True)
class RelayStats:
    opens: int = 0
    connected: int = 0
    failed: dict[int, int] = field(default_factory=dict)   # by pipe error code
    bytes_up: int = 0
    bytes_down: int = 0


@dataclass(frozen=True, slots=True)
class RelayInfo:
    """A stream snapshot for status output (no destination)."""

    stream: int
    kind: str
    state: str
    bytes_up: int
    bytes_down: int


class TcpRelay:
    """The relay endpoint. ``log`` gets one line per event (no destinations); ``resolver``
    is injectable for tests."""

    def __init__(self, *, log: Callable[[str], None] = lambda _line: None,
                 resolver: Resolver = system_resolver) -> None:
        self._log = log
        self._resolver = resolver
        self._session: Session | None = None
        self._conns: dict[int, _Conn] = {}
        self.stats = RelayStats()

    # ---- wiring ---------------------------------------------------------------------------

    def attach(self, session: Session) -> None:
        """Serve this session's relay streams (a new BLE session: anything older is closed)."""
        self.detach()
        self._session = session

    def detach(self) -> None:
        """Close every socket and forget the session."""
        for conn in list(self._conns.values()):
            self._close(conn)
        self._conns.clear()
        self._session = None

    def shutdown(self) -> None:
        """Detach (the app is quitting). A lookup still running finishes on its daemon thread
        and is ignored; it never delays the exit."""
        self.detach()

    def end_all(self, code: int = Error.SERVICE_OFF) -> int:
        """End every live relay stream with ``code`` (OPEN_ERR or RESET, as the session decides):
        the relay switch went off. Returns how many ended."""
        if self._session is None:
            return 0
        ended = 0
        for stream in list(self._conns):
            if self._session.relay_fail(stream, code):
                ended += 1
        return ended

    def streams(self) -> list[RelayInfo]:
        return [RelayInfo(c.stream, _KIND_TEXT.get(c.target.kind, "?"), c.state.value, c.bytes_up, c.bytes_down)
                for c in self._conns.values()]

    # ---- inner.RelayEndpoint --------------------------------------------------------------

    def relay_open(self, stream: int, target: RelayTarget, now: int) -> int | None:
        self.stats.opens += 1
        conn = _Conn(stream, target, ConnState.RESOLVING)
        self._conns[stream] = conn
        self._log(f"relay: stream {stream} OPEN to an {_KIND_TEXT.get(target.kind, '?')} target")
        if target.kind is AddressType.DNS:
            conn.lookup = self._lookup(target.host_text(), target.port)
            return CONNECTING
        family = socket.AF_INET if target.kind is AddressType.IPV4 else socket.AF_INET6
        sockaddr = (target.host_text(), target.port) if family == socket.AF_INET else \
            (target.host_text(), target.port, 0, 0)
        return self._connect(conn, family, sockaddr, now)

    def relay_data(self, stream: int) -> None:
        """P4 bytes or the P4's FIN are waiting: :meth:`poll` moves them (no I/O here)."""

    def relay_end(self, stream: int, end: StreamEnd) -> None:
        conn = self._conns.pop(stream, None)
        if conn is None:
            return
        self._close(conn)
        if end.how is EndHow.REFUSED or end.how is EndHow.LOCAL_RESET:
            self.stats.failed[end.code] = self.stats.failed.get(end.code, 0) + 1
        self._log(f"relay: stream {stream} ended ({end.how.value}, code {end.code}; "
                  f"{conn.bytes_down} B to TCP, {conn.bytes_up} B to the P4)")

    # ---- the main-thread pump ---------------------------------------------------------------

    def poll(self, now: int) -> None:
        """Advance lookups, connects and both byte directions. Call every tick."""
        for conn in list(self._conns.values()):
            if self._conns.get(conn.stream) is not conn:
                continue  # ended by an earlier step of this poll
            match conn.state:
                case ConnState.RESOLVING:
                    self._poll_lookup(conn, now)
                case ConnState.CONNECTING:
                    self._poll_connect(conn, now)
                case ConnState.OPEN:
                    self._pump(conn)

    # ---- internals ------------------------------------------------------------------------

    def _lookup(self, host: str, port: int) -> Future:
        """Run the resolver on a daemon thread; the Future carries its answer or its exception.
        (A ThreadPoolExecutor's workers are joined at interpreter exit, so one hung
        ``getaddrinfo`` would hold up the tool's exit.)"""
        future: Future = Future()
        future.set_running_or_notify_cancel()
        resolver = self._resolver

        def run() -> None:
            try:
                answer = resolver(host, port)
            except BaseException as error:  # noqa: BLE001 - handed to the main thread
                future.set_exception(error)
            else:
                future.set_result(answer)
        threading.Thread(target=run, name="relay-dns", daemon=True).start()
        return future

    def _fail(self, conn: _Conn, code: Error, why: str) -> None:
        self._log(f"relay: stream {conn.stream} {why} (error {int(code)})")
        session = self._session
        if session is None or not session.relay_fail(conn.stream, code):
            # The session no longer tracks it: just drop the socket.
            self._conns.pop(conn.stream, None)
            self._close(conn)

    def _poll_lookup(self, conn: _Conn, now: int) -> None:
        lookup = conn.lookup
        if lookup is None or not lookup.done():
            return
        conn.lookup = None
        try:
            addresses = lookup.result()
        except Exception:  # noqa: BLE001 - any resolver failure is a failed lookup (6)
            addresses = []
        if not addresses:
            self._fail(conn, Error.DNS_FAILED, "name lookup failed")
            return
        family, sockaddr = addresses[0]  # one address, one connect (as the app)
        answer = self._connect(conn, family, sockaddr, now)
        if answer is not CONNECTING and answer != ACCEPT:
            self._fail(conn, Error(answer), "connect failed")

    def _connect(self, conn: _Conn, family: int, sockaddr: tuple, now: int) -> int | None:
        """Start the non-blocking connect: CONNECTING, or CONNECT_FAILED at once."""
        try:
            sock = socket.socket(family, socket.SOCK_STREAM)
        except OSError:
            return Error.CONNECT_FAILED
        sock.setblocking(False)
        try:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except OSError:
            pass
        conn.sock = sock
        conn.state = ConnState.CONNECTING
        code = sock.connect_ex(sockaddr)
        if code not in _IN_PROGRESS:
            self._close(conn)
            return Error.CONNECT_FAILED
        return CONNECTING

    def _poll_connect(self, conn: _Conn, now: int) -> None:
        sock = conn.sock
        assert sock is not None
        _, writable, broken = select.select([], [sock], [sock], 0)
        if not writable and not broken:
            return
        error = sock.getsockopt(socket.SOL_SOCKET, socket.SO_ERROR)
        if error:
            self._fail(conn, Error.CONNECT_FAILED, "connect failed")
            return
        conn.state = ConnState.OPEN
        self.stats.connected += 1
        self._log(f"relay: stream {conn.stream} connected")
        if self._session is not None:
            self._session.relay_connected(conn.stream, now)

    def _pump(self, conn: _Conn) -> None:
        session, sock = self._session, conn.sock
        if session is None or sock is None:
            return
        if not self._downstream(conn, session, sock):
            return
        self._upstream(conn, session, sock)

    def _downstream(self, conn: _Conn, session: Session, sock: socket.socket) -> bool:
        """P4 -> TCP. False when the stream failed."""
        if not conn.out:
            data = session.relay_read(conn.stream)
            if data:
                conn.out += data
        if conn.out:
            try:
                sent = sock.send(conn.out)
            except BlockingIOError:
                sent = 0
            except OSError:
                self._fail(conn, Error.TCP_IO, "send failed")
                return False
            del conn.out[:sent]
            conn.bytes_down += sent
            self.stats.bytes_down += sent
        if not conn.out and not conn.shut_wr and session.relay_shutdown_due(conn.stream):
            try:
                sock.shutdown(socket.SHUT_WR)
            except OSError:
                pass
            conn.shut_wr = True
            session.relay_output_shut(conn.stream)
        return True

    def _upstream(self, conn: _Conn, session: Session, sock: socket.socket) -> None:
        """TCP -> P4, within the session's buffer space."""
        if conn.eof_in:
            return
        space = session.relay_space(conn.stream)
        if space <= 0:
            return
        try:
            data = sock.recv(min(space, RECV_CHUNK))
        except BlockingIOError:
            return
        except OSError:
            self._fail(conn, Error.TCP_IO, "receive failed")
            return
        if not data:
            conn.eof_in = True
            session.relay_eof(conn.stream)
            return
        taken = session.relay_send(conn.stream, data)
        conn.bytes_up += taken
        self.stats.bytes_up += taken

    @staticmethod
    def _close(conn: _Conn) -> None:
        if conn.lookup is not None:
            conn.lookup.cancel()
            conn.lookup = None
        if conn.sock is not None:
            try:
                conn.sock.close()
            except OSError:
                pass
            conn.sock = None
        conn.out[:] = bytes(len(conn.out))
        conn.out.clear()
