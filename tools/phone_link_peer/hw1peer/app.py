"""The Mac test phone: the app stack above the radio, the command sources and ``main``.

Layers (each one testable without Bluetooth; only ``gatt_macos`` touches the radio):

* :class:`Link` -- one BLE connection: the outer channel (:class:`~hw1peer.channel.PhoneChannel`),
  the inner session (:class:`~hw1peer.inner.Session`), the TX gate and the one notification
  that may be waiting for the stack (CoreBluetooth's ``updateValue`` said NO). It starts on the
  P4's CCCD write (``on_subscribe``) by sending HELLO, and ends when the session is over, the
  channel fails, the P4 unsubscribes, or a watchdog fires.
* :class:`ConsoleAdapter` -- the session's console sink over :class:`~hw1peer.records.ConsoleEndpoint`:
  prints INFO, TEXT, CARD, MIRROR, BLOB and ABORT; reports a malformed record so the session
  answers with stream RESET 1.
* :class:`Peer` -- the engine: GATT events, the 10 ms tick, advertising, settings and commands.
  Main thread only; other threads talk to it through :class:`Inbox`.
* :class:`Output` -- stdout plus ``peer.log``, every line masked (:func:`mask`).
* ``main`` -- NSRunLoop on the main thread (``gatt_macos.run_main_loop``), a REPL thread on
  stdin, and a thread tailing ``cmd.in`` (one command per line, for Claude). It holds a lock
  file so only one instance runs, and an NSProcessInfo activity so the Mac does not idle-sleep.

Secrets: the pairing code or passphrase is typed with echo off (``getpass``), turned into the
32-byte PSK at once and only the PSK is kept, in RAM, until ``forget`` or exit. It is never
printed or logged; ``login`` prompts the same way and its line is never logged either. A
command line is echoed (and logged) only when its verb is one of the tool's commands, and
then with the firmware's redaction rules applied, so a secret typed in the wrong place is
never shown. Lines that still contain a login, a pairing code, a ``phone secret`` and the
like are masked, and the log file (mode 0600) also masks IP and MAC addresses and SSID
values (best effort).
"""

import argparse
import contextlib
import fcntl
import getpass
import ipaddress
import os
import queue
import re
import signal
import sys
import threading
import time
import traceback
from collections import deque
from collections.abc import Callable, Iterable, Iterator
from dataclasses import dataclass, field
from enum import Enum
from pathlib import Path
from typing import Final, Protocol, TextIO

from hw1peer import inner, paircode, records
from hw1peer.channel import Data, Established, Invalid, PhoneChannel, Rejected, Reply
from hw1peer.gatt import DEFAULT_LOCAL_NAME, MAX_PRIMARY_NAME, NotifyResult, Peripheral, RadioState, primary_name
from hw1peer.inner import (CONSOLE_CLOSE_TEXT, ConsoleClose, EndHow, EndKind, Error, Session, SessionEnd,
                           StreamEnd, TxGate)
from hw1peer.relay import TcpRelay

TOOL_DIR: Final = Path(__file__).resolve().parents[1]   # tools/phone_link_peer
DEFAULT_LOG: Final = TOOL_DIR / "peer.log"
DEFAULT_CMD_FILE: Final = TOOL_DIR / "cmd.in"
DEFAULT_LOCK: Final = TOOL_DIR / ".hw1peer.lock"   # one instance at a time (git-ignored)

TICK_S: Final = 0.010
HANDSHAKE_DEADLINE_MS: Final = 6_000   # the P4 gives up after 5 s (BLE_PhoneGatt.cpp secure cap)
NOTIFY_STUCK_MS: Final = 5_000         # a refused notification not taken for this long: the link is dead
LINK_SILENCE_MS: Final = 75_000        # no P4 value for this long while the pipe runs (the P4 PINGs after 60 s idle)
ENDED_GRACE_MS: Final = 6_000          # after our session ends, the P4 normally drops the link within this
ADVERTISE_RETRY_MS: Final = 5_000
QUIT_GRACE_MS: Final = 1_500           # how long quit waits for our session RESET to go out
QUIT_FLUSH_MS: Final = 300             # then this long for the stack to transmit it before the service goes
SERVICE_RETRY_MS: Final = 5_000        # a refused service add is retried after this long ...
SERVICE_RETRY_LIMIT: Final = 5         # ... this many times, then the user is told what to do
HANGUP_GRACE_S: Final = 3.0            # after SIGHUP (the Terminal window closed): the clean quit's budget
P4_IDLE_NOTICE_S: Final = 65           # a P4 left holding a link notices it is dead within this (60 s + 5 s PING)

# --------------------------------------------------------------------------------------------
# Masking
# --------------------------------------------------------------------------------------------

MASK: Final = "***"
_LOGIN: Final = re.compile(r"(?i)\b(login)(\s+)(\S+)(\s+)(\S+)")
_CODE_TOKEN: Final = re.compile(r"[0-9A-Za-z](?:[0-9A-Za-z-]*[0-9A-Za-z])?")
_MAC: Final = re.compile(r"(?i)\b[0-9a-f]{2}(?:[:-][0-9a-f]{2}){5}\b")
_IPV4: Final = re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
_IPV6: Final = re.compile(r"(?i)(?<![0-9a-f:])[0-9a-f]{0,4}(?::[0-9a-f]{0,4}){2,7}(?![0-9a-f:])")
#: A dotted quad right after one of these is a version (the P4's INFO "fw", "fw 0.99.98.4", "v1.2.3.4").
_VERSION_LEAD: Final = re.compile(r"""(?i)(?:\bv|\bfw|\bfirmware|\bversion)["']?\s*[:=]?\s*["']?$""")
#: ``ssid: "x"`` / ``SSID='x'`` / JSON ``"ssid":"x"``, and an unquoted ``SSID: My Net`` field.
_SSID_QUOTED: Final = re.compile(r"""(?i)(\bssid\b["']?\s*[:=]?\s*)("[^"]*"|'[^']*')""")
_SSID_FIELD: Final = re.compile(r"""(?i)(\bssid\b\s*[:=]\s*)([^"'\s,;|][^,;|]*)""")
#: Lines where a single-quoted string is an SSID: the firmware's Wi-Fi messages quote it
#: (``wifiscan`` rows ``  3) 'x'  RSSI=-40  BSSID=..``, ``Connecting to 'x'``, ``connected to 'x'``,
#: saved-network rows ``  1. [2] 'x'``, ``Saved network 'x'``, ``Priority updated for 'x'``).
_WIFI_LINE: Final = re.compile(r"(?i)\b(?:b?ssid|rssi|networks?|connect(?:ing|ed)?\s+to|retrying|priority|wi-?fi)\b")
_LIST_ROW: Final = re.compile(r"^(?:P4[^>]{0,8}>)?\s*\d+[.)]\s+(?:\[\d+\]\s+)?'")   # after a "P4> " prefix
_SINGLE_QUOTED: Final = re.compile(r"'[^'\n]*'")

#: (command words, first token to mask (1-based), whole rest?) -- the firmware's own redaction
#: table (System_Utils.cpp kRules) for command lines, plus wifiadd's SSID and llmhttpurl's host.
_COMMAND_RULES: Final[tuple[tuple[tuple[str, ...], int, bool], ...]] = (
    (("login",), 2, True),               # login <user> <password>: both
    (("wifiadd",), 2, True),             # <ssid> <password>
    (("llmhttptoken",), 2, True),
    (("llmhttpurl",), 2, True),
    (("mqttpassword",), 2, True),
    (("testencryption",), 2, True),
    (("testpassword",), 2, True),
    (("otapin",), 2, True),
    (("userrequest",), 3, True),
    (("espnowremote",), 3, True),
    (("espnowroomcmd",), 3, True),
    (("espnowtagcmd",), 3, True),
    (("espnowbrowse",), 3, True),
    (("espnowfetch",), 3, True),
    (("phone", "secret"), 3, True),
    (("code",), 2, True),                # this tool's `code`: anything after it is a secret
    (("blesecret",), 2, True),
    (("espnowsetpassphrase",), 3, True),
    (("espnowmeshes", "setpassphrase"), 4, True),
    (("userchangepassword",), 2, True),
    (("userresetpassword",), 3, True),
    (("useradd",), 3, False),
    (("usersync",), 3, True),
    (("dictate", "result"), 3, True),
    (("dictate", "fail"), 3, True),
    (("automation", "add"), 3, True),
    (("automationadd",), 2, True),
)


def _code_shaped(group: list[re.Match[str]]) -> bool:
    """Tokens that could spell a code: groups of 4 symbols (dashes aside), 16 in all. Prose
    such as "stream 2 connected" also normalises to 16 symbols and passes the check symbol
    one time in 32, so only the code's own shapes (one run, or 4-symbol groups) count."""
    sizes = [len(m.group(0).replace("-", "")) for m in group]
    return sum(sizes) == paircode.SYMBOLS and (len(group) == 1 or all(size % 4 == 0 for size in sizes))


def _mask_codes(text: str) -> str:
    """Hide anything that parses as a pairing code (any case; one run, or 4-symbol groups)."""
    tokens = list(_CODE_TOKEN.finditer(text))
    if not tokens:
        return text
    spans: list[tuple[int, int]] = []
    i = 0
    while i < len(tokens):
        for width in (4, 3, 2, 1):
            group = tokens[i:i + width]
            if (len(group) == width and _code_shaped(group)
                    and paircode.is_code(text[group[0].start():group[-1].end()])):
                spans.append((group[0].start(), group[-1].end()))
                i += width
                break
        else:
            i += 1
    for start, end in reversed(spans):
        text = text[:start] + "****-****-****-****" + text[end:]
    return text


def _mask_ipv6(text: str) -> str:
    def repl(m: re.Match[str]) -> str:
        candidate = m.group(0)
        if candidate.count(":") < 2:
            return candidate
        try:
            ipaddress.IPv6Address(candidate)
        except ValueError:
            return candidate
        return MASK
    return _IPV6.sub(repl, text)


def _mask_ipv4(text: str) -> str:
    """Dotted quads, except a version right after ``fw``/``v``/``version`` (the P4's firmware
    version 0.99.98.4 is a valid dotted quad)."""
    def repl(m: re.Match[str]) -> str:
        return m.group(0) if _VERSION_LEAD.search(text, 0, m.start()) else MASK
    return _IPV4.sub(repl, text)


def _mask_ssids(text: str) -> str:
    text = _SSID_QUOTED.sub(lambda m: m.group(1) + MASK, text)
    text = _SSID_FIELD.sub(lambda m: m.group(1) + MASK, text)
    if _WIFI_LINE.search(text) or _LIST_ROW.match(text):
        text = _SINGLE_QUOTED.sub(f"'{MASK}'", text)
    return text


def mask(text: str, *, for_file: bool = False, login: bool = True) -> str:
    """A line made safe to show (``for_file``: to write to a file). Hides
    ``login <user> <password>`` (unless ``login`` is False, for the tool's own fixed text) and
    pairing codes; for files also MAC, IPv4, IPv6 addresses and SSID values. Best effort: it
    cannot recognise a free-form passphrase."""
    if login:
        text = _LOGIN.sub(lambda m: f"{m.group(1)}{m.group(2)}{MASK}{m.group(4)}{MASK}", text)
    text = _mask_codes(text)
    if for_file:
        text = _MAC.sub(MASK, text)
        text = _mask_ipv4(text)
        text = _mask_ipv6(text)
        text = _mask_ssids(text)
    return text


def redact_command(line: str) -> str:
    """A CLI line as it may be logged: the firmware's command redaction rules (also behind this
    tool's ``cmd`` prefix), then :func:`mask`."""
    words = line.split()
    lead: list[str] = []
    if words and words[0].lower() == "cmd":
        lead, words = words[:1], words[1:]
    lowered = [w.lower() for w in words]
    for prefix, first, rest in _COMMAND_RULES:
        if tuple(lowered[:len(prefix)]) != prefix or len(words) < first:
            continue
        head = words[:first - 1]
        tail = [MASK] if rest else [MASK] + words[first:]
        return mask(" ".join(lead + head + tail))
    return mask(line)


# --------------------------------------------------------------------------------------------
# Output
# --------------------------------------------------------------------------------------------

def open_private_log(path: Path) -> TextIO:
    """The log for appending, readable by this user only (0600, also when it already existed)."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    try:
        os.fchmod(fd, 0o600)
        return os.fdopen(fd, "a", encoding="utf-8")
    except BaseException:
        os.close(fd)
        raise


class Output:
    """Printed lines (stdout) and the log file, masked; safe to call from any thread. Not from
    a signal handler: the lock is not reentrant."""

    def __init__(self, log_path: Path | None, *, stream: TextIO | None = None,
                 wall: Callable[[], float] = time.time) -> None:
        self._stream = stream if stream is not None else sys.stdout
        self._wall = wall
        self._lock = threading.Lock()
        self._file = open_private_log(log_path) if log_path is not None else None
        self._screen = True   # False once the terminal is gone (closed window): the log goes on

    def say(self, text: str, *, fixed: bool = False) -> None:
        """Show and log. ``fixed`` marks the tool's own constant text (help, banners): it skips
        the ``login`` mask, which would otherwise eat the help's own ``login`` line."""
        self._emit(text, screen=True, fixed=fixed)

    def note(self, text: str) -> None:
        """Log only (for example the echo of a line the user just typed)."""
        self._emit(text, screen=False)

    def close(self) -> None:
        with self._lock:
            if self._file is not None:
                self._file.close()
                self._file = None

    def _emit(self, text: str, *, screen: bool, fixed: bool = False) -> None:
        lines = text.splitlines() or [""]
        stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(self._wall()))
        with self._lock:
            for line in lines:
                if screen and self._screen:
                    try:
                        print(mask(line, login=not fixed), file=self._stream, flush=True)
                    except (OSError, ValueError):
                        self._screen = False
                if self._file is not None:
                    self._file.write(f"{stamp} {mask(line, for_file=True, login=not fixed)}\n")
            if self._file is not None:
                self._file.flush()


# --------------------------------------------------------------------------------------------
# The secret
# --------------------------------------------------------------------------------------------

@dataclass(eq=False)
class Credentials:
    """The pipe PSK (RAM only) and whether it came from a pairing code (§6.2 #7: only a code
    carries the console). The typed secret itself is not kept."""

    psk: bytearray = field(repr=False)
    is_code: bool

    def wipe(self) -> None:
        self.psk[:] = bytes(len(self.psk))


_REFUSALS: Final = {
    paircode.EntryFault.INVALID_UTF8: "not valid UTF-8",
    paircode.EntryFault.CONTROL: "it contains a control character",
    paircode.EntryFault.TOO_SHORT: "fewer than 10 bytes (and not a pairing code)",
    paircode.EntryFault.TOO_LONG: "more than 128 bytes",
    paircode.EntryFault.NO_UPPER: "a passphrase needs an uppercase letter (or type a pairing code)",
    paircode.EntryFault.NO_LOWER: "a passphrase needs a lowercase letter (or type a pairing code)",
    paircode.EntryFault.NO_DIGIT: "a passphrase needs a digit (or type a pairing code)",
    paircode.EntryFault.NO_PUNCT: "a passphrase needs punctuation (or type a pairing code)",
}


def credentials_from_entry(entry: str | bytes) -> Credentials | str:
    """What the user typed at ``code``, as credentials, or a refusal message that never
    contains the entry. A valid pairing code in any spelling is stored canonically (§5.5)."""
    result = paircode.pipe_secret(entry)
    if isinstance(result, paircode.EntryRefused):
        return f"Not accepted: {_REFUSALS.get(result.fault, result.fault.value)}."
    try:
        return Credentials(bytearray(result.psk()), result.is_code)
    finally:
        result.wipe()


# --------------------------------------------------------------------------------------------
# The console stream
# --------------------------------------------------------------------------------------------

def describe_stream_end(end: StreamEnd) -> str:
    code = end.code
    match end.how:
        case EndHow.PEER_CLOSE:
            text = CONSOLE_CLOSE_TEXT.get(ConsoleClose(code)) if 0 <= code <= ConsoleClose.REVOKED else ""
            name = ConsoleClose(code).name.lower() if 0 <= code <= ConsoleClose.REVOKED else str(code)
            return f"the P4 closed it (CLOSE {code} {name}{': ' + text if text else ''})"
        case EndHow.PEER_RESET:
            return f"the P4 reset it ({_error_name(code)})"
        case EndHow.LOCAL_CLOSE:
            return f"we closed it (CLOSE {code})"
        case EndHow.LOCAL_RESET:
            return f"we reset it ({_error_name(code)})"
        case EndHow.REFUSED:
            return f"we refused it ({_error_name(code)})"
        case EndHow.SESSION:
            return "the pipe session ended"
    return end.how.value


def _error_name(code: int) -> str:
    return f"{code} {Error(code).name.lower()}" if inner.known_error(code) else str(code)


def describe_end(end: SessionEnd) -> str:
    match end.kind:
        case EndKind.FAILED:
            return f"failed: we refused something from the P4 ({_error_name(end.error)})"
        case EndKind.PEER_RESET:
            return f"the P4 ended it (RESET {_error_name(end.error)})"
        case EndKind.LOCAL_RESET:
            return f"we ended it (RESET {_error_name(end.error)})"
        case EndKind.VERSION:
            return "we sent the version signal (RESET 2 under a v1 header): a mismatch strike"
        case EndKind.LINK_LOST:
            return "the link went away"
    return end.kind.value


class ConsoleAdapter:
    """The session's :class:`~hw1peer.inner.ConsoleSink`: P4 records in, printed lines out."""

    def __init__(self, show: Callable[[str], None]) -> None:
        self._show = show
        self.endpoint = records.ConsoleEndpoint()
        self.stream = 0          # the open console stream id (0: none)
        self.opens = 0

    @property
    def info(self) -> records.Info | None:
        return self.endpoint.info

    # ---- inner.ConsoleSink ----------------------------------------------------------------

    def on_console_open(self, stream: int) -> None:
        self.endpoint.reset()
        self.stream = stream
        self.opens += 1
        self._show(f"console: open (stream {stream}); the P4 signs it out: type login")

    def on_console_bytes(self, data: bytes) -> bool:
        ok = True
        for event in self.endpoint.feed(data):
            ok &= not isinstance(event, records.Malformed)
            self._event(event)
        return ok

    def on_console_end(self, end: StreamEnd) -> None:
        for event in self.endpoint.flush():
            self._event(event)
        self.stream = 0
        self._show(f"console: closed: {describe_stream_end(end)}")

    # ---- the way back ---------------------------------------------------------------------

    def command(self, line: str) -> bytes:
        """One COMMAND record (raises records.RecordError for an empty, long or multi-line one)."""
        return self.endpoint.command(line)

    # ---- printing -------------------------------------------------------------------------

    def _lines(self, prefix: str, text: str) -> None:
        for line in text.splitlines() or [""]:
            self._show(prefix + line)

    def _event(self, event: records.Event) -> None:
        match event:
            case records.Info():
                self._show("P4 INFO> " + event.text)
            case records.Text():
                self._lines("P4> ", event.text)
            case records.Card():
                self._show("P4 CARD> " + event.text)
            case records.Mirror():
                self._lines("P4 MIRROR> ", event.text)
            case records.Blob():
                fit = "matches" if event.length_matches else "does NOT match"
                head = event.header_text if event.header is not None else "(no header)"
                self._show(f"P4 BLOB> {len(event.data)} bytes; header {head}; length {fit}")
            case records.Abort():
                self._show(f"P4 ABORT> {records.type_name(event.chain_type)} chain cut after "
                           f"{len(event.partial)} bytes (output not confirmed)")
                if event.chain_type == records.RecordType.TEXT and event.partial:
                    self._lines("P4 (cut)> ", event.partial.decode("utf-8", errors="replace"))
            case records.Malformed():
                self._show(f"console: malformed record from the P4 ({event.reason}); "
                           "answering with stream RESET 1")


# --------------------------------------------------------------------------------------------
# One BLE link
# --------------------------------------------------------------------------------------------

class Phase(Enum):
    HANDSHAKE = "secure handshake"
    PIPE = "pipe"
    ENDED = "ended"       # our side is over; waiting for the P4 to drop the link


class LinkHost(Protocol):
    """What a :class:`Link` needs from the engine."""

    def say(self, text: str) -> None: ...

    def notify(self, central: str, value: bytes) -> NotifyResult: ...

    def new_session(self, max_frame: int, now: int) -> Session: ...

    def session_ended(self, end: SessionEnd) -> None: ...


@dataclass(slots=True)
class LinkStats:
    values_in: int = 0
    bytes_in: int = 0
    values_out: int = 0
    bytes_out: int = 0
    busy: int = 0          # notifications the stack refused (queue full), each retried
    ignored: int = 0       # P4 values after our side ended


@dataclass(slots=True)
class _Held:
    """A value the stack refused: sent as is on the next ready-to-update (never re-sealed)."""

    value: bytes
    frame: bool     # a session frame: accept() it once the stack takes it
    since: int


class Link:
    """One BLE connection's protocol stack (see the module docstring)."""

    def __init__(self, host: LinkHost, central: str, max_update: int, psk: bytearray, now: int, *,
                 random: Callable[[int], bytes] = os.urandom) -> None:
        self.host = host
        self.central = central
        self.max_update = max_update
        self.max_frame = inner.frame_limit_for_update_length(max_update)
        self.channel = PhoneChannel(psk, random=random)
        self.session: Session | None = None
        self.gate = TxGate()
        self.phase = Phase.HANDSHAKE
        self.started = self.last_rx = now
        self.ended_at: int | None = None
        self.why = ""
        self.stats = LinkStats()
        self._outbox: deque[bytes] = deque()
        self._held: _Held | None = None
        self._reported = False
        self._announced = False
        self._p4_flags: int | None = None
        self._refused: dict[int, int] = {}
        self._link_states_out = 0
        #: Our side ended a link the P4 still holds and nothing could tell it (no frame
        #: possible): the engine removes and re-adds the service so the P4 sees Service
        #: Changed and stops at once, instead of after its idle PING (up to about 65 s).
        self.kick_pending = False

    @property
    def mtu(self) -> int:
        return self.max_update + 3

    @property
    def waiting(self) -> bool:
        """A refused notification is waiting for the stack."""
        return self._held is not None

    def begin(self, now: int) -> None:
        """The P4 subscribed: the phone speaks first (HELLO)."""
        self._outbox.append(self.channel.hello())
        self.pump(now)

    # ---- P4 -> phone ----------------------------------------------------------------------

    def on_write(self, value: bytes, now: int) -> None:
        self.stats.values_in += 1
        self.stats.bytes_in += len(value)
        if self.phase is Phase.ENDED:
            self.stats.ignored += 1
            return
        self.last_rx = now
        match self.channel.receive(value):
            case Reply(value=confirm):
                self._outbox.append(confirm)
            case Established():
                self.session = self.host.new_session(self.max_frame, now)
                self.phase = Phase.PIPE
                self.host.say("secure channel up: waiting for the P4's pipe HELLO")
            case Data(plaintext=plain):
                assert self.session is not None  # Data only comes once Established
                self.session.receive(plain, now)
            case Rejected(reason=reason):
                self._end(now, f"the P4 refused the handshake (REJECT {reason}): "
                               "the pairing code or passphrase does not match the P4's")
                return
            case Invalid(why=why):
                # Once the pipe runs the P4 cannot hear about it (the channel is gone): kick it.
                self._end(now, f"secure channel failed ({why})", kick=self.phase is Phase.PIPE)
                return
        self._after_session(now)
        self.pump(now)

    # ---- timers ---------------------------------------------------------------------------

    def tick(self, now: int) -> None:
        if self.phase is Phase.ENDED:
            return
        if self.phase is Phase.HANDSHAKE and now - self.started >= HANDSHAKE_DEADLINE_MS:
            self._end(now, "the secure handshake timed out")
            return
        if self._held is not None and now - self._held.since >= NOTIFY_STUCK_MS:
            self._end(now, "the stack took no notification for 5 s: the link is gone",
                      kick=self.phase is Phase.PIPE)
            return
        if self.phase is Phase.PIPE and now - self.last_rx >= LINK_SILENCE_MS:
            self._end(now, f"nothing from the P4 for {LINK_SILENCE_MS // 1000} s: the link is gone", kick=True)
            return
        if self.session is not None:
            self.session.tick(now)
            self._after_session(now)
        self.pump(now)

    # ---- phone -> P4 ----------------------------------------------------------------------

    def pump(self, now: int) -> None:
        """At most one notification, when the gate allows (30 ms apart, one at a time)."""
        if self.phase is Phase.ENDED or self._held is not None or not self.gate.ready(now):
            return
        if self._outbox:
            self._submit(self._outbox.popleft(), False, now)
            return
        session = self.session
        if session is None or session.done:
            return
        frame = session.peek(now)
        if frame is None:
            self._after_session(now)  # peek can fail a session (sequence exhausted)
            return
        value = self.channel.seal(frame)
        if value is None:
            self._end(now, "the channel's send counter is exhausted", kick=True)
            return
        self._submit(value, True, now)

    def ready_to_update(self, now: int) -> None:
        held = self._held
        if held is None or self.phase is Phase.ENDED:
            return
        if held.frame and (self.session is None or self.session.done):
            self._held = None
            self.gate.completed()
            return
        self.gate.submitted(now)  # the 30 ms gap counts from the actual hand-over
        match self.host.notify(self.central, held.value):
            case NotifyResult.SENT:
                self._held = None
                self._sent(held.value, held.frame, now)
            case NotifyResult.BUSY:
                self.stats.busy += 1
            case NotifyResult.GONE:
                self._end(now, "the P4 is no longer subscribed")

    def _submit(self, value: bytes, frame: bool, now: int) -> None:
        self.gate.submitted(now)
        match self.host.notify(self.central, value):
            case NotifyResult.SENT:
                self._sent(value, frame, now)
            case NotifyResult.BUSY:
                self.stats.busy += 1
                self._held = _Held(value, frame, now)
            case NotifyResult.GONE:
                self._end(now, "the P4 is no longer subscribed")

    def _sent(self, value: bytes, frame: bool, now: int) -> None:
        self.gate.completed()
        self.stats.values_out += 1
        self.stats.bytes_out += len(value)
        if frame and self.session is not None:
            self.session.accept(now)
            self._after_session(now)

    # ---- endings --------------------------------------------------------------------------

    def lost(self, now: int, why: str) -> None:
        """The P4 unsubscribed or the radio went: nothing more can be sent."""
        if self.phase is not Phase.ENDED:
            self._end(now, why)

    def abandon(self, now: int, why: str) -> None:
        """End the link from our side at once (for example the secret was forgotten). In the
        pipe phase no frame can tell the P4 before READY, so it is kicked (Service Changed);
        during the secure handshake the P4's own 5 s cap ends it."""
        if self.phase is not Phase.ENDED:
            self._end(now, why, kick=self.phase is Phase.PIPE)

    def _after_session(self, now: int) -> None:
        """Report what the session did (READY, flags, refusals) and finish when it is over."""
        session = self.session
        if session is None or self.phase is not Phase.PIPE:
            return
        self._report(session)
        if not session.done:
            return
        end = session.end
        assert end is not None
        if end.kind is EndKind.FAILED and (frame := session.failure_frame()) is not None:
            value = self.channel.seal(frame)
            if value is not None and self.host.notify(self.central, value) is NotifyResult.SENT:
                self.stats.values_out += 1
                self.stats.bytes_out += len(value)
            else:
                self.kick_pending = True  # the RESET could not go out: the P4 would not know
        self._finish(now, describe_end(end))

    def _report(self, session: Session) -> None:
        if session.ready and not self._announced:
            self._announced = True
            self.host.say(f"pipe READY: frame limit {session.value}; the P4 "
                          f"{'offers' if session.console_offered else 'does not offer'} the console; "
                          f"our flags: {flag_text(session.local_flags)}")
        elif session.end is not None and session.end.kind is EndKind.VERSION and not self._announced:
            self._announced = True
            self.host.say("answered the P4's HELLO with the version signal (mismatch mode)")
        if session.ready:
            flags = session.p4_flags
            if self._p4_flags is not None and flags != self._p4_flags:
                self.host.say(f"P4 LINK_STATE: it {'offers' if flags & inner.CONSOLE_OFFERED else 'no longer offers'} "
                              "the console")
            self._p4_flags = flags
        for code, count in session.stats.refused.items():
            for _ in range(count - self._refused.get(code, 0)):
                self.host.say(f"refused an OPEN from the P4 with {_error_name(code)}")
        self._refused = dict(session.stats.refused)
        if session.stats.link_states_out > self._link_states_out:
            self._link_states_out = session.stats.link_states_out
            self.host.say(f"sent LINK_STATE: {flag_text(session.local_flags)}")

    def _end(self, now: int, why: str, *, kick: bool = False) -> None:
        """Our side ends without the session's own ending (channel failure, watchdog, link gone).
        ``kick``: the P4 still holds the link and nothing more can be sent to tell it."""
        if self.session is not None and not self.session.done:
            self.session.link_lost()
        self.kick_pending = kick and self.phase is not Phase.ENDED
        self._finish(now, why)

    def _finish(self, now: int, why: str) -> None:
        if self.phase is Phase.ENDED:
            return
        self.phase = Phase.ENDED
        self.ended_at = now
        self.why = why
        self._outbox.clear()
        self._held = None
        self.gate.completed()
        self.channel.wipe()
        if self.session is not None and not self._reported and self.session.end is not None:
            self._reported = True
            self.host.session_ended(self.session.end)
        self.host.say(f"link ended: {why}")


def flag_text(flags: int) -> str:
    """The phone's flag byte in words."""
    return (f"relay {'on' if flags & inner.RELAY_ON else 'off'}, "
            f"console {'wanted' if flags & inner.CONSOLE_WANTED else 'not wanted'}")


# --------------------------------------------------------------------------------------------
# Commands from other threads
# --------------------------------------------------------------------------------------------

class RequestKind(Enum):
    LINE = "line"        # a command line (REPL or cmd.in)
    SECRET = "secret"    # new credentials from the REPL's `code`
    LOGIN = "login"      # a `login <user> <password>` line from the REPL's `login` (never logged)
    NOTICE = "notice"    # a fixed line to print (from a signal handler, which must not print itself)
    QUIT = "quit"


@dataclass(frozen=True)
class Request:
    kind: RequestKind
    source: str = "repl"
    text: str = field(default="", repr=False)
    creds: Credentials | None = field(default=None, repr=False)


class Inbox:
    """Thread-safe hand-over from the REPL, the cmd.in tail and signals to the main thread."""

    def __init__(self) -> None:
        self._queue: queue.SimpleQueue[Request] = queue.SimpleQueue()

    def put(self, request: Request) -> None:
        self._queue.put(request)

    def drain(self) -> list[Request]:
        out: list[Request] = []
        while True:
            try:
                out.append(self._queue.get_nowait())
            except queue.Empty:
                return out


# --------------------------------------------------------------------------------------------
# The engine
# --------------------------------------------------------------------------------------------

@dataclass
class Settings:
    relay_on: bool = False       # RELAY_ON: relay OPENs go to real TCP (relay.py)
    console_wanted: bool = True  # CONSOLE_WANTED (only with a pairing code, §6.2 #7)
    mismatch: bool = False       # answer every new session's HELLO with the version signal
    advertise: bool = True


HELP: Final = """\
Commands (type them here, or append lines to cmd.in):
  status               link, session, console and relay state
  stats                counters
  code                 type the pairing code or passphrase (hidden; keyboard only)
  login [user]         sign in on the P4 console (password hidden; keyboard only; never logged)
  cmd <line>           send one CLI line to the P4 console (replies print as "P4> ...")
  relay on|off         RELAY_ON: relay OPENs go to real TCP, or are refused with 16
  console on|off       CONSOLE_WANTED (a passphrase secret never carries the console)
  adv on|off           advertise the pipe service
  mismatch on|off      answer each new session's HELLO with the version signal (a strike)
  ping                 send a PING to the P4
  end [code]           end the pipe session with a stream-0 RESET (default 18 paused)
  forget               drop the secret from RAM, end the link, stop advertising
  quit                 end the session (RESET 18), stop advertising, remove the service, exit
At a hidden prompt, Ctrl-D cancels the prompt; Ctrl-C elsewhere quits (twice: at once)."""

def split_verb(line: str) -> tuple[str, str]:
    """(lowercase first word, the rest stripped); any whitespace separates them."""
    parts = line.split(None, 1)
    if not parts:
        return "", ""
    return parts[0].lower(), parts[1].strip() if len(parts) > 1 else ""


#: The verbs above: only these lines are echoed or logged (anything else could be a secret
#: typed in the wrong place, so it is never shown).
VERBS: Final = frozenset({"help", "?", "status", "stats", "code", "login", "cmd", "relay", "console", "adv",
                          "mismatch", "ping", "end", "forget", "quit", "exit"})


class Peer:
    """The engine. Implements :class:`hw1peer.gatt.PeripheralEvents` and :class:`LinkHost`.
    Everything runs on the main thread: GATT events, :meth:`tick` and the queued commands."""

    def __init__(self, peripheral: Peripheral, out: Output, *, relay: TcpRelay | None = None,
                 settings: Settings | None = None, clock: Callable[[], int] | None = None,
                 random: Callable[[int], bytes] = os.urandom) -> None:
        self.peripheral = peripheral
        self.out = out
        self.relay = relay if relay is not None else TcpRelay(log=out.say)
        self.settings = settings or Settings()
        self.clock = clock or (lambda: time.monotonic_ns() // 1_000_000)
        self.random = random
        self.inbox = Inbox()
        self.console = ConsoleAdapter(out.say)
        self.creds: Credentials | None = None
        self.link: Link | None = None
        self.radio = RadioState.UNKNOWN
        self.service_ready = False
        self.advertising = False
        self.advertise_error: str | None = None
        self._advertise_sent: bool | None = None
        self._advertise_retry_at = 0
        self.quitting = False
        self.finished = False
        self._quit_at = 0
        self._quit_flush_at: int | None = None
        self.service_error: str | None = None
        self._service_retry_at: int | None = None
        self.service_retries = 0
        self.kicks = 0
        # history
        self.links = 0
        self.ends: dict[str, int] = {}
        self.strikes_sent = 0
        self.last_end = ""
        self.ignored_subscriptions = 0

    # ---- lifecycle ------------------------------------------------------------------------

    def start(self) -> None:
        self.peripheral.start(self)

    def tick(self) -> None:
        """The 10 ms heartbeat: commands, relay sockets, the link, advertising, quitting."""
        now = self.clock()
        for request in self.inbox.drain():
            self._handle(request, now)
        self.relay.poll(now)
        link = self.link
        if link is not None:
            link.tick(now)
            self._kick(link)
            if link.phase is Phase.ENDED and link.ended_at is not None and now - link.ended_at >= ENDED_GRACE_MS:
                self._drop_link()
        self._retry_service(now)
        self._refresh_advertising(now)
        self._quit_progress(now)

    def session(self) -> Session | None:
        return self.link.session if self.link is not None and self.link.phase is Phase.PIPE else None

    # ---- gatt.PeripheralEvents --------------------------------------------------------------

    def on_radio(self, state: RadioState) -> None:
        self.radio = state
        if state is not RadioState.POWERED_ON:
            self.service_ready = self.advertising = False
            self._advertise_sent = None
            self._service_retry_at = None   # a power-on adds the service afresh
            self.service_retries = 0
        match state:
            case RadioState.UNAUTHORIZED:
                self.out.say("Bluetooth permission denied: allow Terminal in System Settings > Privacy & "
                             "Security > Bluetooth, then start the tool again.")
            case RadioState.UNSUPPORTED:
                self.out.say("This Mac cannot act as a Bluetooth LE peripheral.")
            case RadioState.POWERED_OFF:
                self.out.say("Bluetooth is off.")
            case RadioState.POWERED_ON:
                self.out.say("Bluetooth is on; adding the pipe service.")
            case _:
                self.out.say(f"Bluetooth state: {state.value}.")

    def on_service_added(self, error: str | None) -> None:
        self.service_ready = error is None
        self.service_error = error
        if error is None:
            self.service_retries = 0
            self._service_retry_at = None
            self.out.say("pipe service added.")
            return
        if self.service_retries < SERVICE_RETRY_LIMIT:
            self._service_retry_at = self.clock() + SERVICE_RETRY_MS
            self.out.say(f"the pipe service was refused: {error} (retrying in {SERVICE_RETRY_MS // 1000} s)")
        else:
            self._service_retry_at = None
            self.out.say(f"the pipe service was refused: {error}. Gave up after {SERVICE_RETRY_LIMIT} retries: "
                         "toggle Bluetooth off and on, or restart the tool.")

    def on_advertising(self, started: bool, error: str | None) -> None:
        self.advertising = started
        if error is not None:
            self.advertise_error = error
            self._advertise_sent = None
            self._advertise_retry_at = self.clock() + ADVERTISE_RETRY_MS
            self.out.say(f"advertising failed: {error} (retrying in {ADVERTISE_RETRY_MS // 1000} s)")
        elif started:
            self.advertise_error = None
            self.out.say("advertising the pipe service: waiting for the P4")
        else:
            self.out.say("advertising stopped")

    def on_subscribe(self, central: str, max_update: int) -> None:
        now = self.clock()
        if self.quitting:
            return
        if self.creds is None:
            self.ignored_subscriptions += 1
            self.out.say("the P4 subscribed but no pairing code is set (type code); it will give up")
            return
        if self.link is not None and self.link.central != central and self.link.phase is not Phase.ENDED:
            self.ignored_subscriptions += 1
            self.out.say("another central subscribed while the pipe is busy: ignored")
            return
        if self.link is not None:
            self._drop_link()
        max_frame = inner.frame_limit_for_update_length(max_update)
        if max_frame < inner.MIN_VALUE:
            self.ignored_subscriptions += 1
            self.out.say(f"the P4 subscribed at ATT MTU {max_update + 3}, below the pipe's 64: ignored")
            return
        self.links += 1
        self.link = Link(self, central, max_update, self.creds.psk, now, random=self.random)
        self.out.say(f"P4 subscribed (ATT MTU {max_update + 3}, frame limit {max_frame}): secure handshake")
        self._refresh_advertising(now)
        self.link.begin(now)

    def on_unsubscribe(self, central: str) -> None:
        link = self.link
        if link is None or link.central != central:
            return
        link.lost(self.clock(), "the P4 unsubscribed or disconnected")
        link.kick_pending = False  # it is gone: nothing to tell it
        self._drop_link()

    def on_write(self, central: str, value: bytes) -> None:
        link = self.link
        if link is not None and link.central == central:
            link.on_write(value, self.clock())
            self._kick(link)

    def on_ready_to_update(self) -> None:
        if self.link is not None:
            self.link.ready_to_update(self.clock())

    # ---- LinkHost ---------------------------------------------------------------------------

    def say(self, text: str) -> None:
        self.out.say(text)

    def notify(self, central: str, value: bytes) -> NotifyResult:
        return self.peripheral.notify(central, value)

    def new_session(self, max_frame: int, now: int) -> Session:
        session = Session(max_frame, now, console=self.console, relay=self.relay,
                          relay_on=self.settings.relay_on, console_wanted=self._console_wanted(),
                          mismatch=self.settings.mismatch, random=self.random)
        self.relay.attach(session)
        return session

    def session_ended(self, end: SessionEnd) -> None:
        key = end.kind.value
        self.ends[key] = self.ends.get(key, 0) + 1
        if end.strike:
            self.strikes_sent += 1
        self.last_end = describe_end(end)
        self.relay.detach()

    # ---- commands ---------------------------------------------------------------------------

    def _handle(self, request: Request, now: int) -> None:
        match request.kind:
            case RequestKind.SECRET:
                assert request.creds is not None
                self._set_secret(request.creds)
            case RequestKind.LOGIN:
                self.out.note("> login (hidden)")
                self._send_console(request.text, now)
            case RequestKind.NOTICE:
                self.out.say(request.text, fixed=True)
            case RequestKind.QUIT:
                self._begin_quit(now)
            case RequestKind.LINE:
                self.command(request.text, now, source=request.source)

    def command(self, line: str, now: int | None = None, *, source: str = "repl") -> None:
        """One command line (see :data:`HELP`)."""
        now = self.clock() if now is None else now
        line = line.strip()
        if not line:
            return
        verb, rest = split_verb(line)
        if verb not in VERBS:
            # Never echo it: a passphrase typed at the wrong prompt lands here.
            where = "" if source == "repl" else f" from {source}"
            self.out.say(f"unknown command{where} (not shown; type help)", fixed=True)
            return
        # The source tag goes first, so the login mask can never take it for a password.
        echo = ("" if source == "repl" else f"[{source}] ") + f"> {redact_command(line)}"
        if source == "repl":
            self.out.note(echo)
        else:
            self.out.say(echo)
        match verb:
            case "help" | "?":
                self.out.say(HELP, fixed=True)
            case "status":
                self.out.say(self.status_text())
            case "stats":
                self.out.say(self.stats_text())
            case "code" | "login":
                self.out.say(f"{verb} is accepted only from the keyboard: secrets never go through files",
                             fixed=True)
            case "cmd":
                if source != "repl" and rest.lower().split()[:1] == ["login"]:
                    self.out.say("not sent: logins only from the keyboard (use the login command)")
                elif not rest:
                    self.out.say("usage: cmd <line>")
                else:
                    self._send_console(rest, now)
            case "relay":
                self._switch(rest, self._set_relay)
            case "console":
                self._switch(rest, self._set_console)
            case "adv":
                self._switch(rest, self._set_advertise)
            case "mismatch":
                self._switch(rest, self._set_mismatch)
            case "ping":
                session = self.session()
                ok = session is not None and session.ping(now)
                self.out.say("PING queued" if ok else "no PING: the pipe is not ready or one is outstanding")
            case "end":
                self._end_session(rest)
            case "forget":
                self._forget()
            case "quit" | "exit":
                self._begin_quit(now)

    @staticmethod
    def _parse_switch(text: str) -> bool | None:
        return {"on": True, "off": False, "1": True, "0": False}.get(text.lower())

    def _switch(self, text: str, apply: Callable[[bool], None]) -> None:
        value = self._parse_switch(text)
        if value is None:
            self.out.say("usage: <relay|console|adv|mismatch> on|off")
        else:
            apply(value)

    def _set_secret(self, creds: Credentials) -> None:
        if self.creds is not None:
            self.creds.wipe()
        self.creds = creds
        if creds.is_code:
            self.out.say("Pairing code accepted (held in RAM only; console allowed).")
        else:
            self.out.say("Passphrase accepted (held in RAM only). A passphrase carries the relay only: "
                         "the console needs a device code from `phone pair new`.")
        if self.link is not None and self.link.phase is not Phase.ENDED:
            self.out.say("The current link keeps its key; the new secret applies from the next connection.")

    def _settings_flags(self) -> int:
        """The flag byte the settings ask for (what the next HELLO_ACK would carry)."""
        relay = inner.RELAY_ON if self.settings.relay_on else 0
        return relay | (inner.CONSOLE_WANTED if self._console_wanted() else 0)

    def _console_wanted(self) -> bool:
        return self.settings.console_wanted and self.creds is not None and self.creds.is_code

    def _set_relay(self, on: bool) -> None:
        self.settings.relay_on = on
        session = self.session()
        if session is not None:
            session.set_relay_on(on)
            ended = 0 if on else self.relay.end_all(Error.SERVICE_OFF)
            note = f"; ended {ended} relay stream(s) with 16" if ended else ""
            self.out.say(f"relay {'on' if on else 'off'}{note}")
        else:
            self.out.say(f"relay {'on' if on else 'off'} (from the next session)")

    def _set_console(self, on: bool) -> None:
        self.settings.console_wanted = on
        session = self.session()
        if session is not None:
            session.set_console_wanted(self._console_wanted())
        self.out.say(f"console {'wanted' if on else 'not wanted'}")
        if on and self.creds is not None and not self.creds.is_code:
            self.out.say("note: the secret is a passphrase, so console OPENs are still refused with 16 (§6.2 #7)")

    def _set_advertise(self, on: bool) -> None:
        self.settings.advertise = on
        self.out.say(f"advertising {'wanted' if on else 'off'}")

    def _set_mismatch(self, on: bool) -> None:
        self.settings.mismatch = on
        session = self.session()
        if session is not None and session.state is inner.State.AWAIT_HELLO:
            session.mismatch = on
        self.out.say("mismatch on: each new session answers the P4's HELLO with the version signal "
                     "(three in a row latch the P4)" if on else "mismatch off")

    def _send_console(self, line: str, now: int) -> None:
        session = self.session()
        if session is None or not session.ready:
            self.out.say("not sent: no pipe session")
            return
        if not self.console.stream:
            why = ("the P4 does not offer it" if not session.console_offered else
                   "we do not want it (console on, and a pairing code)" if not self._console_wanted() else
                   "it is not open yet")
            self.out.say(f"not sent: the console stream is not open ({why})")
            return
        try:
            record = self.console.command(line)
        except records.RecordError as error:
            self.out.say(f"not sent: {error}")
            return
        if not session.console_send(record):
            self.out.say(f"not sent: the console is busy ({session.console_space()} B free); try again")
            return
        assert self.link is not None
        self.link.pump(now)

    def _end_session(self, text: str) -> None:
        session = self.session()
        try:
            code = int(text) if text else int(Error.PAUSED)
        except ValueError:
            code = -1
        if not inner.known_error(code):
            self.out.say("usage: end [code 1..18]")
        elif session is None or not session.end_session(code):
            self.out.say("no pipe session to end")
        else:
            self.out.say(f"ending the pipe session with RESET {_error_name(code)}")

    def _forget(self) -> None:
        if self.creds is not None:
            self.creds.wipe()
            self.creds = None
        self.out.say("secret forgotten; advertising stops")
        link = self.link
        if link is None or link.phase is Phase.ENDED:
            return
        session = self.session()
        if session is not None and session.ready and session.end_session(Error.PAUSED):
            self.out.say("ending the pipe session with RESET 18 paused")
            return
        # Not READY yet: the link keeps a copy of the old key and must never reach READY with it.
        link.abandon(self.clock(), "secret forgotten")
        self._kick(link)

    # ---- advertising and quitting -------------------------------------------------------------

    def _refresh_advertising(self, now: int) -> None:
        want = (self.settings.advertise and self.creds is not None and self.radio is RadioState.POWERED_ON
                and self.service_ready and self.link is None and not self.quitting)
        if self._advertise_sent is None and want and now < self._advertise_retry_at:
            return
        if want != self._advertise_sent:
            self._advertise_sent = want
            self.peripheral.set_advertising(want)

    def _drop_link(self) -> None:
        link, self.link = self.link, None
        if link is not None:
            link.lost(self.clock(), "dropped")

    def _kick(self, link: Link) -> None:
        """Our side ended a link the P4 still holds, with no frame able to tell it: remove and
        re-add the service so the P4 sees Service Changed and stops (BLE_PhoneGatt.cpp
        SRVC_CHG -> stop(Protocol)). Without that the P4 notices only through its idle PING."""
        if not link.kick_pending or link.phase is not Phase.ENDED:
            return
        link.kick_pending = False
        if self.finished or self.radio is not RadioState.POWERED_ON:
            return
        self.kicks += 1
        self.peripheral.reset_service()
        self.out.say("re-added the pipe service so the P4 sees Service Changed and stops now; if it misses "
                     f"that, it notices within about {P4_IDLE_NOTICE_S} s (its idle PING)")

    def _retry_service(self, now: int) -> None:
        """Retry a refused service add every SERVICE_RETRY_MS, SERVICE_RETRY_LIMIT times."""
        at = self._service_retry_at
        if at is None or now < at or self.service_ready or self.finished:
            return
        if self.radio is not RadioState.POWERED_ON:
            return  # the next power-on adds it again
        self._service_retry_at = None
        self.service_retries += 1
        self.out.say(f"adding the pipe service again (retry {self.service_retries} of {SERVICE_RETRY_LIMIT})")
        self.peripheral.reset_service()

    def _begin_quit(self, now: int) -> None:
        if self.quitting:
            return
        self.quitting = True
        self._quit_at = now
        session = self.session()
        if session is not None and session.ready and session.end_session(Error.PAUSED):
            self.out.say("quitting: ending the pipe session (RESET 18 paused)")
        else:
            self.out.say("quitting")

    def _quit_progress(self, now: int) -> None:
        if not self.quitting or self.finished:
            return
        session = self.session()
        if session is not None and not session.done and now - self._quit_at < QUIT_GRACE_MS:
            return
        if self._quit_flush_at is None:
            self._quit_flush_at = now
        if now - self._quit_flush_at < QUIT_FLUSH_MS:
            return  # "taken by the stack" is not "on the air": let the run loop transmit it
        self.close()

    def close(self) -> None:
        """Stop everything: the link, the relay, advertising and the service; wipe the secret."""
        if self.finished:
            return
        self.finished = True
        try:
            self._drop_link()
            self.relay.shutdown()
            self.peripheral.close()
        finally:
            if self.creds is not None:  # whatever failed above, the secret goes
                self.creds.wipe()
                self.creds = None
        self.out.say("stopped advertising and removed the pipe service")

    # ---- status -------------------------------------------------------------------------------

    def status_text(self) -> str:
        creds = self.creds
        secret = ("pairing code (console allowed)" if creds and creds.is_code else
                  "passphrase (relay only)" if creds else "none: type code")
        service = ("added" if self.service_ready else
                   f"refused ({self.service_error})" if self.service_error else "not added")
        lines = [
            f"secret: {secret}",
            f"bluetooth: {self.radio.value}; service {service}; "
            f"advertising {'yes' if self.advertising else 'no'}"
            + (f" (last error: {self.advertise_error})" if self.advertise_error else ""),
            f"settings: {flag_text(self._settings_flags())}; "
            f"mismatch {'on' if self.settings.mismatch else 'off'}; version signals sent {self.strikes_sent}",
        ]
        link = self.link
        if link is None:
            lines.append("link: none")
        else:
            lines.append(f"link: {link.phase.value}; ATT MTU {link.mtu}; frame limit {link.max_frame}"
                         + (f"; {link.why}" if link.phase is Phase.ENDED else ""))
            session = link.session
            if session is not None and link.phase is Phase.PIPE:
                lines.append(f"session: {session.state.value}; value {session.value}; P4 "
                             f"{'offers' if session.console_offered else 'does not offer'} the console; "
                             f"our flags: {flag_text(session.local_flags)}")
                for s in session.streams():
                    lines.append(f"  stream {s.stream}: {s.kind.value} {s.state.value}; sent {s.sent} B, "
                                 f"received {s.received} B")
        info = self.console.info
        if self.console.stream:
            fields = info.fields if info is not None else None
            about = (f"fw {fields.get('fw')}, model {fields.get('model')}" if fields else "no INFO yet")
            lines.append(f"console: open (stream {self.console.stream}); {about}")
        else:
            lines.append("console: not open")
        relay_streams = self.relay.streams()
        lines.append(f"relay: {'on' if self.settings.relay_on else 'off'}; live streams {len(relay_streams)}")
        for r in relay_streams:
            lines.append(f"  relay stream {r.stream}: {r.kind} {r.state}; to TCP {r.bytes_down} B, "
                         f"to the P4 {r.bytes_up} B")
        if self.last_end:
            lines.append(f"last session end: {self.last_end}")
        return "\n".join(lines)

    def stats_text(self) -> str:
        lines = [f"links {self.links}; ignored subscriptions {self.ignored_subscriptions}; "
                 f"session ends {self.ends or '{}'}; version signals sent {self.strikes_sent}"]
        link = self.link
        if link is not None:
            st = link.stats
            lines.append(f"link: values in {st.values_in} ({st.bytes_in} B), out {st.values_out} ({st.bytes_out} B); "
                         f"stack busy {st.busy}; ignored {st.ignored}; channel tx counter {link.channel.tx_counter}, "
                         f"rx counter {link.channel.rx_counter}")
            if link.session is not None:
                ss = link.session.stats
                lines.append(f"session: frames in {ss.frames_in}, out {ss.frames_out}; opens {ss.opens}; "
                             f"refused {ss.refused}; resets {ss.local_resets}; peer closes {ss.peer_closes}; "
                             f"peer resets {ss.peer_resets}; LINK_STATE in {ss.link_states_in}, "
                             f"out {ss.link_states_out}; "
                             f"PING in {ss.pings_in}, out {ss.pings_out}")
                lines.append(f"bytes: console in {ss.console_bytes_in}, out {ss.console_bytes_out}; "
                             f"relay in {ss.relay_bytes_in}, out {ss.relay_bytes_out}")
        cs = self.console.endpoint.stats
        lines.append(f"console: opens {self.console.opens}; records {cs.records}; messages {cs.messages}; "
                     f"skipped {cs.skipped}; aborts {cs.aborts}; commands {cs.commands_built}")
        rs = self.relay.stats
        lines.append(f"relay: opens {rs.opens}; connected {rs.connected}; failed {rs.failed}; "
                     f"to TCP {rs.bytes_down} B; to the P4 {rs.bytes_up} B")
        return "\n".join(lines)


# --------------------------------------------------------------------------------------------
# Command sources
# --------------------------------------------------------------------------------------------

class CommandFileTail:
    """Reads lines appended to a command file (``cmd.in``) after the tool started. A truncated
    file is read again from the start. Partial lines wait for their newline."""

    def __init__(self, path: Path) -> None:
        self.path = path
        path.touch(exist_ok=True)
        self._pos = path.stat().st_size   # old commands are never replayed
        self._partial = b""

    def poll(self) -> list[str]:
        try:
            size = self.path.stat().st_size
        except FileNotFoundError:
            self._pos, self._partial = 0, b""
            return []
        if size < self._pos:
            self._pos, self._partial = 0, b""
        if size == self._pos:
            return []
        with self.path.open("rb") as handle:
            handle.seek(self._pos)
            chunk = handle.read(size - self._pos)
        self._pos += len(chunk)
        data = self._partial + chunk
        *lines, self._partial = data.split(b"\n")
        return [line.decode("utf-8", errors="replace").rstrip("\r") for line in lines]


class PromptGuard:
    """Whether a hidden prompt (``code``, ``login``) is open on the REPL thread.

    Python runs signal handlers on the main thread only, so Ctrl-C can never interrupt the
    REPL's ``getpass``: it would quit the whole tool instead. While a prompt is open, the first
    Ctrl-C only tells the user that Ctrl-D cancels the prompt; a second one quits. The state
    is two Events, so the signal handler reads it without taking a lock."""

    def __init__(self) -> None:
        self._open = threading.Event()
        self._warned = threading.Event()

    @contextlib.contextmanager
    def prompt(self) -> Iterator[None]:
        self._warned.clear()
        self._open.set()
        try:
            yield
        finally:
            self._open.clear()

    @property
    def is_open(self) -> bool:
        return self._open.is_set()

    def first_interrupt(self) -> bool:
        """For the SIGINT handler: True once per prompt (the interrupt only warns)."""
        if self._open.is_set() and not self._warned.is_set():
            self._warned.set()
            return True
        return False


PROMPT_HINT: Final = "Ctrl-D cancels the prompt; press Ctrl-C again to quit the tool."
LOGIN_INLINE: Final = "Not sent: type `login` or `login <user>`; the password is then asked with echo off."
CODE_INLINE: Final = ("Ignored what followed `code` (not shown): type the secret at the hidden prompt, "
                      "never on the command line.")


def read_code(inbox: Inbox, out: Output, read_secret: Callable[[str], str]) -> None:
    """The REPL's `code`: read with echo off, derive the PSK here, hand only the PSK over."""
    entry = read_secret("Pairing code or passphrase (hidden): ")
    result = credentials_from_entry(entry)
    del entry
    if isinstance(result, str):
        out.say(result)
    else:
        inbox.put(Request(RequestKind.SECRET, creds=result))


def read_login(rest: str, inbox: Inbox, out: Output, read_line: Callable[[str], str],
               read_secret: Callable[[str], str]) -> None:
    """The REPL's `login [user]`: the password is read with echo off; the line is never logged.
    A password typed on the command line (echo on) is refused, never sent or shown."""
    words = rest.split()
    if len(words) >= 2:
        out.say(LOGIN_INLINE, fixed=True)
        return
    user = words[0] if words else read_line("user: ").strip()
    password = read_secret("password (hidden): ")
    if not user or not password or any(c.isspace() for c in user + password):
        out.say("Not sent: the user and the password must be non-empty, without spaces.", fixed=True)
        return
    inbox.put(Request(RequestKind.LOGIN, text=f"login {user} {password}"))


def repl(inbox: Inbox, out: Output, *, read_line: Callable[[str], str] = input,
         read_secret: Callable[[str], str] = getpass.getpass, prompts: PromptGuard | None = None) -> None:
    """Read commands from the keyboard until EOF (which quits). Runs on its own thread, where
    Ctrl-C never arrives (see :class:`PromptGuard`); Ctrl-D cancels a hidden prompt."""
    prompts = prompts if prompts is not None else PromptGuard()
    while True:
        try:
            line = read_line("")
        except EOFError:
            inbox.put(Request(RequestKind.QUIT))
            return
        verb, rest = split_verb(line)
        if verb not in ("code", "login"):
            inbox.put(Request(RequestKind.LINE, "repl", line))
            continue
        try:
            with prompts.prompt():
                if verb == "code":
                    if rest:
                        out.say(CODE_INLINE, fixed=True)
                    read_code(inbox, out, read_secret)
                else:
                    read_login(rest, inbox, out, read_line, read_secret)
        except EOFError:
            out.say("cancelled", fixed=True)


def tail_commands(path: Path, inbox: Inbox, stop: threading.Event, period_s: float = 0.2) -> None:
    tail = CommandFileTail(path)
    while not stop.wait(period_s):
        for line in tail.poll():
            inbox.put(Request(RequestKind.LINE, path.name, line))


class Interrupts:
    """main's signal handling. A handler runs on the main thread between two bytecodes, maybe
    while that thread holds Output's lock, so it only puts requests in the inbox and sets
    flags; the engine prints and acts on its next tick.

    * SIGINT (Ctrl-C) and SIGTERM: quit cleanly (RESET 18 first); a second one while quitting
      stops at once. Ctrl-C at an open hidden prompt first only says that Ctrl-D cancels it.
    * SIGHUP (the Terminal window closed): quit cleanly too, but stop at once if that has not
      finished within :data:`HANGUP_GRACE_S`.
    """

    def __init__(self, inbox: Inbox, quitting: Callable[[], bool], prompts: PromptGuard, *,
                 clock: Callable[[], float] = time.monotonic) -> None:
        self._inbox = inbox
        self._quitting = quitting
        self._prompts = prompts
        self._clock = clock
        self.force = threading.Event()
        self.deadline: float | None = None

    def handle(self, signum: int, _frame: object = None) -> None:
        if signum == signal.SIGHUP and self.deadline is None:
            self.deadline = self._clock() + HANGUP_GRACE_S
        if self._quitting():
            self.force.set()
        elif signum == signal.SIGINT and self._prompts.first_interrupt():
            self._inbox.put(Request(RequestKind.NOTICE, text=PROMPT_HINT))
        else:
            self._inbox.put(Request(RequestKind.QUIT, source=signal.Signals(signum).name))

    def should_stop(self) -> bool:
        return self.force.is_set() or (self.deadline is not None and self._clock() >= self.deadline)

    def install(self) -> None:
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
            signal.signal(signum, self.handle)


def acquire_instance_lock(path: Path) -> TextIO | None:
    """An exclusive lock held for the process's lifetime (flock: released when the process
    ends, however it ends), or None if another instance holds it. Two instances would publish
    the service twice (the P4 then refuses every attempt) and both run every cmd.in line."""
    handle = path.open("a", encoding="utf-8")
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        handle.close()
        return None
    except BaseException:
        handle.close()
        raise
    return handle


# --------------------------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------------------------

def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="python -m hw1peer",
        description="A Mac 'test phone' for the HardwareOne phone link v2 (BLE peripheral). Run it in Terminal.app.")
    parser.add_argument("--relay", action="store_true", help="start with RELAY_ON (relay OPENs go to real TCP)")
    parser.add_argument("--no-console", action="store_true", help="start with CONSOLE_WANTED off")
    parser.add_argument("--name", default=None,
                        help="advertised local name, at most 8 bytes so it fits beside the service UUID "
                             "(default HW1Peer; '' for none)")
    parser.add_argument("--log", type=Path, default=DEFAULT_LOG, help=f"log file (default {DEFAULT_LOG.name})")
    parser.add_argument("--cmd-file", type=Path, default=DEFAULT_CMD_FILE,
                        help=f"command file tailed for extra commands (default {DEFAULT_CMD_FILE.name})")
    parser.add_argument("--no-repl", action="store_true",
                        help="no keyboard commands (stdin is not read): drive it through the command file; "
                             "`quit` there, Ctrl-C or SIGTERM stop it")
    parser.add_argument("--lock-file", type=Path, default=DEFAULT_LOCK, help=argparse.SUPPRESS)
    return parser


def _step(out: Output | None, what: str, call: Callable[[], object]) -> None:
    """One shutdown step: a failure is reported, never raised (it must not skip the next steps
    or hide the exception that ended the run)."""
    try:
        call()
    except Exception:  # noqa: BLE001 - reported, the next steps still run
        text = f"shutdown: {what} failed:\n{traceback.format_exc()}"
        try:
            if out is None:
                raise RuntimeError("no output")
            out.say(text, fixed=True)
        except Exception:  # noqa: BLE001
            print(text, file=sys.stderr)


def main(argv: Iterable[str] | None = None) -> int:
    args = build_parser().parse_args(list(argv) if argv is not None else None)
    out = Output(args.log)
    lock = acquire_instance_lock(args.lock_file)
    if lock is None:
        out.say("another hw1peer is running (one instance at a time: two would publish the pipe service "
                "twice); quit it first.", fixed=True)
        out.close()
        return 2
    try:
        from hw1peer import gatt_macos
    except ImportError as error:
        out.say(f"CoreBluetooth (pyobjc) is not available: {error}")
        out.close()
        lock.close()
        return 2
    name = DEFAULT_LOCAL_NAME if args.name is None else args.name
    if name and primary_name(name) is None:
        out.say(f"--name is longer than {MAX_PRIMARY_NAME} bytes: advertising without a name")
    peripheral = gatt_macos.MacPeripheral(name=name, on_error=out.say)
    peer = Peer(peripheral, out, settings=Settings(relay_on=args.relay, console_wanted=not args.no_console))
    prompts = PromptGuard()
    interrupts = Interrupts(peer.inbox, lambda: peer.quitting, prompts)
    interrupts.install()
    stop_tail = threading.Event()
    if not args.no_repl:
        threading.Thread(target=repl, args=(peer.inbox, out), kwargs={"prompts": prompts}, name="repl",
                         daemon=True).start()
    threading.Thread(target=tail_commands, args=(args.cmd_file, peer.inbox, stop_tail), name="cmd-file",
                     daemon=True).start()
    out.say("HardwareOne Mac test phone (phone link v2). Type help. "
            f"Log: {args.log.name}; command file: {args.cmd_file.name}.")
    out.say("No secret yet: run `phone pair new` on the P4, then type code here.")
    terminal = _save_terminal()
    awake: Callable[[], None] | None = None
    try:
        awake = gatt_macos.hold_awake("HardwareOne phone link test (hw1peer)")
        peer.start()
        gatt_macos.run_main_loop(peer.tick, lambda: peer.finished or interrupts.should_stop(), TICK_S)
    finally:
        stop_tail.set()
        _step(out, "closing the peer", peer.close)
        if awake is not None:
            _step(out, "ending the keep-awake activity", awake)
        _step(None, "closing the log", out.close)
        _step(None, "restoring the terminal", lambda: _restore_terminal(terminal))
        _step(None, "releasing the instance lock", lock.close)
    return 0


def _save_terminal() -> object | None:
    """The terminal's settings: a getpass prompt cut short by quitting would leave echo off."""
    try:
        import termios
        return termios.tcgetattr(sys.stdin.fileno()) if sys.stdin.isatty() else None
    except (ImportError, OSError, ValueError):
        return None


def _restore_terminal(saved: object | None) -> None:
    if saved is None:
        return
    try:
        import termios
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, saved)
    except (ImportError, OSError, ValueError):
        pass
