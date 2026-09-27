"""Serial transport for the isolated HardwareOne mesh experiment; no test actions.

Requires pyserial only when a real port is opened. The caller supplies a new log
path under its private, timestamped run directory and registers *all* credentials
in ``secrets`` before sending them. Logs (mode 0600) and ``on_log`` callbacks are
redacted; returned responses remain raw in memory for assertions. Call ``redact``
before printing or otherwise persisting returned data.

Example (opening this class never resets or flashes a board)::

    with MeshConsole(port, private_run / "s3.log", secrets=[password]) as board:
        board.read_until(r"Enter admin username", since=0, timeout=60)
        mark = board.send_line(username)  # wizard input, no shell prompt expected
        board.read_until(r"Enter admin password", since=mark)
        # Once setup is complete, synchronize on the initial shell prompt.
        response = board.command("whoami")

Markers are absolute received-byte offsets. ``read_until`` does not consume data;
reuse a marker to inspect the same response. The bounded in-memory buffer rejects
expired markers explicitly. Default command completion uses a new ``$ `` prompt.
For provisioned HardwareOne consoles, use ``completion="hardwareone"``: each
command queues a read-only ``whoami`` FIFO barrier and ignores all prompts. The
barrier needs an authenticated console (or a successful login command). A timeout
does not cancel firmware work; its eventual barrier must finish before reuse.
"""

from __future__ import annotations

import datetime as dt
import json
import os
from pathlib import Path
import re
import threading
import time
from typing import Callable, Iterable, Pattern


ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
# The prompt starts a line, but asynchronous logs may immediately follow its
# trailing space (there is no newline). A '[serial] $ command' audit line or a
# dollar sign inside a JSON string must not finish command().
PROMPT = re.compile(r"(?m)^\$ ")
WHOAMI_LINE = r"^(?:\$ )*You are [^\r\n]+\r?\n"
RESULT_PREFIX = re.compile(r"^(?:\$ )?\[(?:serial|uart)(?: [^\]\r\n]*)?\] ")


class ConsoleTimeout(TimeoutError):
    """The raw partial response is available as .output, never in the message."""

    def __init__(self, since: int, timeout: float, output: str):
        super().__init__(f"Serial response timed out after {timeout:g}s (marker {since})")
        self.output = output
        self.since = since


def extract_json_objects(text: str) -> list[dict]:
    """Return complete top-level objects amid prompts, ANSI and log prefixes.

    Handles the current byte-exact serial results and older results split across
    repeated ``[serial user@local] `` lines, including splits inside JSON strings.
    Nested objects are not returned separately. An incomplete outer object is
    ignored rather than reporting one of its inner objects as a complete result.
    This function neither logs nor prints the (potentially sensitive) objects.
    """
    clean = ANSI.sub("", text)
    # Old queued results add a newline and the same prefix to each physical
    # fragment. Join only adjacent fragments with the identical result prefix.
    lines = clean.splitlines(keepends=True)
    normalized: list[str] = []
    previous_prefix: str | None = None
    for line in lines:
        match = RESULT_PREFIX.match(line)
        prefix = match.group(0) if match else None
        if prefix and prefix == previous_prefix and normalized:
            normalized[-1] = normalized[-1].rstrip("\r\n") + line[match.end():]
        else:
            normalized.append(line[match.end():] if match else line)
        previous_prefix = prefix
    clean = "".join(normalized)

    decoder = json.JSONDecoder()
    objects: list[dict] = []
    start = 0
    while True:
        start = clean.find("{", start)
        if start < 0:
            break
        # Locate a balanced outer object, respecting quoted braces and escapes.
        depth, quoted, escaped = 0, False, False
        end = None
        for pos in range(start, len(clean)):
            char = clean[pos]
            if quoted:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    quoted = False
            elif char == '"':
                quoted = True
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    end = pos + 1
                    break
        if end is None:
            # A non-JSON diagnostic containing an unmatched brace must not hide
            # a later JSON response; only treat a plausible object as partial.
            if re.match(r'\{\s*(?:"|$)', clean[start:]):
                break
            start += 1
            continue
        try:
            value = decoder.decode(clean[start:end])
        except json.JSONDecodeError:
            pass
        else:
            if isinstance(value, dict):
                objects.append(value)
        start = end
    return objects


class MeshConsole:
    def __init__(
        self,
        port: str,
        log_path: str | Path,
        *,
        secrets: Iterable[str] = (),
        baudrate: int = 115200,
        chunk_size: int = 64,
        chunk_delay: float = 0.020,
        max_buffer_bytes: int = 8 * 1024 * 1024,
        on_log: Callable[[str], None] | None = None,
        serial_factory: Callable | None = None,
        completion: str = "prompt",
    ):
        if completion not in ("prompt", "hardwareone"):
            raise ValueError("Completion must be prompt or hardwareone")
        if not 1 <= chunk_size <= 64 or chunk_delay < 0.020:
            raise ValueError("Use at most 64 bytes per chunk and at least 20 ms pacing")
        if max_buffer_bytes < 1024:
            raise ValueError("Receive buffer must hold at least 1024 bytes")
        values = tuple(secrets)
        if any(not isinstance(s, str) or not s or "\n" in s or "\r" in s for s in values):
            raise ValueError("Secrets must be nonempty, single-line strings")
        self._secrets = tuple(sorted(set(values), key=len, reverse=True))
        self._secret_pattern = re.compile("|".join(re.escape(s) for s in self._secrets)) if values else None
        self._condition = threading.Condition()
        self._command_lock = threading.Lock()
        self._close_lock = threading.Lock()
        self._log_lock = threading.Lock()
        self._stop = threading.Event()
        self._buffer = bytearray()
        self._base = 0
        self._max_buffer = max_buffer_bytes
        self._reader_error: Exception | None = None
        self._chunk_size = chunk_size
        self._chunk_delay = chunk_delay
        self._on_log = on_log
        self._completion = completion
        self._closed = False
        self._port = None
        self._reader = None
        self.log_path = Path(log_path)
        self.log_path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        # No overwrite or append: a caller cannot accidentally mix two runs.
        descriptor = os.open(self.log_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        self._log = os.fdopen(descriptor, "w", encoding="utf-8", buffering=1)
        try:
            if serial_factory is None:
                import serial
                serial_factory = serial.Serial
            self._port = serial_factory(port=None, baudrate=baudrate, timeout=0.1, write_timeout=5)
            self._port.dtr = False
            self._port.rts = False
            self._port.port = port
            self._port.open()
            self._reader = threading.Thread(target=self._read_loop, name="mesh-console-reader", daemon=True)
            self._reader.start()
        except BaseException:
            if self._port is not None:
                self._port.close()
            self._log.close()
            raise

    def redact(self, text: str) -> str:
        clean = ANSI.sub("", text)
        return self._secret_pattern.sub("[REDACTED]", clean) if self._secret_pattern else clean

    def _log_line(self, direction: str, text: str) -> None:
        # Never log a raw repr(), exception, command, or incomplete RX fragment.
        stamp = dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")
        safe = f"{stamp} {direction} {self.redact(text)}"
        with self._log_lock:
            if self._log.closed:
                return
            self._log.write(safe + "\n")
        if self._on_log is not None:
            self._on_log(safe)

    def _read_loop(self) -> None:
        pending = bytearray()
        dropping_line = False
        try:
            while not self._stop.is_set():
                data = self._port.read(min(max(self._port.in_waiting, 1), 4096))
                if not data:
                    continue
                with self._condition:
                    self._buffer.extend(data)
                    excess = len(self._buffer) - self._max_buffer
                    if excess > 0:
                        del self._buffer[:excess]
                        self._base += excess
                    self._condition.notify_all()
                pending.extend(data)
                while b"\n" in pending:
                    line, _, remainder = pending.partition(b"\n")
                    pending = bytearray(remainder)
                    if not dropping_line:
                        self._log_line("RX", line.decode("utf-8", "replace").rstrip("\r"))
                    dropping_line = False
                # Omit huge unterminated lines whole; splitting one for logging
                # could expose a secret that straddles the artificial boundary.
                if len(pending) > 65536:
                    pending.clear()
                    if not dropping_line:
                        self._log_line("RX", "[overlong unterminated line omitted]")
                    dropping_line = True
        except Exception as exc:
            if not self._stop.is_set():
                with self._condition:
                    self._reader_error = exc
                    self._condition.notify_all()
        # Drop the partial final line, which might be an incomplete credential.

    def marker(self) -> int:
        with self._condition:
            return self._base + len(self._buffer)

    def _since_locked(self, since: int) -> str:
        if since < self._base:
            raise ValueError("Receive marker expired; increase max_buffer_bytes or take a new marker")
        if since > self._base + len(self._buffer) or since < 0:
            raise ValueError("Receive marker is outside the observed stream")
        return bytes(self._buffer[since - self._base:]).decode("utf-8", "replace")

    def read_since(self, since: int) -> str:
        """Return a raw, non-consuming snapshot; redact before logging/printing."""
        with self._condition:
            return self._since_locked(since)

    def read_until(self, pattern: str | Pattern[str], *, since: int | None = None, timeout: float = 10) -> str:
        """Wait using a monotonic deadline; return the raw snapshot when matched."""
        if timeout < 0:
            raise ValueError("Timeout must be nonnegative")
        expression = re.compile(pattern) if isinstance(pattern, str) else pattern
        deadline = time.monotonic() + timeout
        with self._condition:
            if since is None:
                since = self._base + len(self._buffer)
            while True:
                output = self._since_locked(since)
                if expression.search(ANSI.sub("", output)):
                    return output
                if self._reader_error is not None:
                    raise ConnectionError("Serial reader failed; raw exception retained in memory") from None
                if self._stop.is_set():
                    raise ConnectionError("Serial console closed while waiting")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise ConsoleTimeout(since, timeout, output)
                self._condition.wait(remaining)

    def _send_line_locked(self, text: str) -> int:
        if "\n" in text or "\r" in text:
            raise ValueError("send_line accepts one line without a terminator")
        if self._stop.is_set():
            raise ConnectionError("Serial console is closed")
        payload = text.encode("utf-8") + b"\n"
        self._log_line("TX", text)
        response_marker = self.marker()
        for offset in range(0, len(payload), self._chunk_size):
            block = payload[offset:offset + self._chunk_size]
            if offset + len(block) == len(payload):
                # No command can complete before its newline. This also excludes
                # old prompts received while a long command was being uploaded.
                response_marker = self.marker()
            written = 0
            while written < len(block):
                try:
                    count = self._port.write(block[written:])
                except Exception:
                    raise ConnectionError("Serial write failed") from None
                if not count:
                    raise ConnectionError("Serial write made no progress")
                written += count
            if offset + len(block) < len(payload) and self._stop.wait(self._chunk_delay):
                raise ConnectionError("Serial console closed during write")
        return response_marker

    def send_line(self, text: str) -> int:
        """Send one paced wizard/login line; return a marker for its response."""
        with self._command_lock:
            return self._send_line_locked(text)

    def command(self, text: str, *, timeout: float = 10) -> str:
        """Send a command and await its completion, holding the command lock.

        HardwareOne mode appends a read-only whoami command. Its complete direct
        serial result is a FIFO barrier after the requested command, independent
        of stale/delayed prompts and async log output. Requesting whoami itself
        expects two identity lines. Returned text includes the barrier line;
        JSON extraction ignores it. Use send_line/read_until for setup input.

        First synchronize with setup/boot using read_until. Timeout covers the
        response wait after the paced upload, which can take about 0.65s for a
        2047-byte command. This method never logs or prints its raw return value.
        """
        with self._command_lock:
            mark = self._send_line_locked(text)
            if self._completion == "hardwareone":
                if self._stop.wait(self._chunk_delay):
                    raise ConnectionError("Serial console closed before completion barrier")
                self._send_line_locked("whoami")
                requested_whoami = re.fullmatch(r'\s*(?:whoami|"whoami")\s*', text, re.I) is not None
                pattern = WHOAMI_LINE + (r"[\s\S]*?" + WHOAMI_LINE if requested_whoami else "")
                return self.read_until(re.compile(pattern, re.M), since=mark, timeout=timeout)
            return self.read_until(PROMPT, since=mark, timeout=timeout)

    def close(self) -> None:
        with self._close_lock:
            if self._closed:
                return
            self._closed = True
            self._stop.set()
            with self._condition:
                self._condition.notify_all()
            cancel = getattr(self._port, "cancel_read", None)
            if cancel is not None:
                try:
                    cancel()
                except Exception:
                    pass
            if self._reader is not None and threading.current_thread() is not self._reader:
                self._reader.join(timeout=1)
            try:
                self._port.close()
            finally:
                if self._reader is not None and threading.current_thread() is not self._reader:
                    self._reader.join(timeout=1)
                with self._log_lock:
                    self._log.close()

    def __enter__(self) -> MeshConsole:
        return self

    def __exit__(self, exc_type, exc_value, traceback) -> None:
        self.close()
