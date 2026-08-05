"""Exclusive, timestamped USB-serial evidence capture with lazy pyserial use."""

from __future__ import annotations

import os
import pathlib
import queue
import re
import threading
import time
from datetime import datetime, timezone
from typing import Any, Callable


SerialFactory = Callable[[str, int, float], Any]


def available_ports() -> list[dict[str, str]]:
    try:
        from serial.tools import list_ports
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for USB observation; run under the ESP-IDF "
            "Python environment or install it explicitly"
        ) from exc
    return [
        {
            "device": item.device,
            "description": item.description or "",
            "hwid": item.hwid or "",
            "vid": "" if item.vid is None else f"{item.vid:04x}",
            "pid": "" if item.pid is None else f"{item.pid:04x}",
            "serialNumber": item.serial_number or "",
        }
        for item in list_ports.comports()
    ]


def _default_factory(port: str, baud: int, timeout: float) -> Any:
    try:
        import serial
    except ImportError as exc:
        raise RuntimeError(
            "pyserial is required for USB observation; run under the ESP-IDF "
            "Python environment or install it explicitly"
        ) from exc
    stream = serial.Serial(port=None, baudrate=baud, timeout=timeout)
    stream.dtr = False
    stream.rts = False
    if hasattr(stream, "exclusive"):
        stream.exclusive = True
    stream.port = port
    stream.open()
    return stream


class SerialObserver:
    def __init__(
        self,
        port: str,
        output: pathlib.Path,
        *,
        baud: int = 115200,
        factory: SerialFactory = _default_factory,
    ):
        if not port:
            raise ValueError("serial port must be explicit")
        self.port = port
        self.output = output
        self.baud = baud
        self.factory = factory
        self._stream: Any = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._lines: queue.Queue[str] = queue.Queue()

    def start(self) -> None:
        if self._thread is not None:
            raise RuntimeError("serial observer is already started")
        self.output.parent.mkdir(parents=True, exist_ok=True)
        self._stream = self.factory(self.port, self.baud, 0.25)
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._capture, name="hw1-ota-serial", daemon=True
        )
        self._thread.start()

    def _capture(self) -> None:
        descriptor = os.open(
            self.output, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600
        )
        with os.fdopen(descriptor, "a", encoding="utf-8") as log:
            while not self._stop.is_set():
                raw = self._stream.readline()
                if not raw:
                    continue
                line = raw.decode("utf-8", "replace").rstrip("\r\n")
                stamp = datetime.now(timezone.utc).isoformat(timespec="milliseconds")
                log.write(f"{stamp} {line}\n")
                log.flush()
                self._lines.put(line)

    def wait_for(self, pattern: str, timeout: float) -> str:
        expression = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"serial pattern not observed: {pattern!r}")
            try:
                line = self._lines.get(timeout=remaining)
            except queue.Empty as exc:
                raise TimeoutError(
                    f"serial pattern not observed: {pattern!r}"
                ) from exc
            if expression.search(line):
                return line

    def close(self) -> None:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=2.0)
        if self._stream is not None:
            self._stream.close()
        self._thread = None
        self._stream = None

    def __enter__(self) -> "SerialObserver":
        self.start()
        return self

    def __exit__(self, *_args: object) -> None:
        self.close()
