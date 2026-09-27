"""Connectivity-only log filtering; raw serial buffers remain untouched.

redact_http_bodies(text) can also clean existing ignored private logs. It never
prints and preserves JSON quoting, including JSON-escaped response strings.
"""
from pathlib import Path
import re
import sys
import threading

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "p4_mesh"))
from console import ConsoleTimeout, MeshConsole, WHOAMI_LINE

_HTTP_BODY = re.compile(r'(?P<quote>\\*")body(?P=quote)\s*:\s*(?P=quote)[A-Za-z0-9+/=]*(?P=quote)')


def redact_http_bodies(text):
    if "s3-http-v1" not in text:
        return text
    def replace(match):
        quote = match.group("quote")
        return quote + "body" + quote + ":" + quote + "[HTTP body omitted]" + quote
    return _HTTP_BODY.sub(replace, text)


class ConnectivityConsoleFatal(ConnectionError):
    """Serial completion is unknown; close/reopen rather than issuing commands."""


class ConnectivityConsole(MeshConsole):
    def __init__(self, *args, recovery_timeout=10, **kwargs):
        self._transaction_lock = threading.RLock()
        self._fatal_console = False
        self._recovery_timeout = recovery_timeout
        super().__init__(*args, **kwargs)

    @property
    def fatal_console(self):
        return self._fatal_console

    def command(self, text, *, timeout=10):
        # Keep an outer transaction lock while draining a timed-out command's
        # original completion marker. MeshConsole releases its command lock
        # when it raises, while the firmware command may still be executing.
        with self._transaction_lock:
            if self._fatal_console:
                raise ConnectivityConsoleFatal("Console completion is unknown; close/reopen before further commands")
            try:
                return super().command(text, timeout=timeout)
            except ConsoleTimeout as error:
                if self._completion != "hardwareone":
                    self._fatal_console = True
                    raise ConnectivityConsoleFatal("Cannot recover a timed-out console without the HardwareOne barrier") from None
                requested_whoami = re.fullmatch(r'\s*(?:whoami|"whoami")\s*', text, re.I) is not None
                pattern = WHOAMI_LINE + (r"[\s\S]*?" + WHOAMI_LINE if requested_whoami else "")
                try:
                    self.read_until(re.compile(pattern, re.M), since=error.since, timeout=self._recovery_timeout)
                except Exception:
                    self._fatal_console = True
                    raise ConnectivityConsoleFatal("Timed-out serial command did not reach its completion barrier; close/reopen required") from None
                # The original test still fails its deadline; only subsequent
                # explicit cleanup commands may now safely use the console.
                raise

    def redact(self, text):
        # Omit the encoded body before literal credential replacement: replacing
        # a substring first could break the base64 grammar and prevent omission.
        return super().redact(redact_http_bodies(text))


def clean_private_log(path):
    """Rewrite one caller-selected ignored private log, returning only a count.

    The caller must choose the file; no directory discovery or device access.
    Never use this against logs that a running coordinator is still writing.
    """
    path = Path(path)
    if "private" not in path.parts or path.is_symlink() or not path.is_file():
        raise ValueError("Expected a regular file inside an experiment private directory")
    before = path.read_text(encoding="utf-8")
    after = redact_http_bodies(before)
    count = after.count("[HTTP body omitted]") - before.count("[HTTP body omitted]")
    if before != after:
        import os
        import tempfile
        fd, temporary = tempfile.mkstemp(prefix=".redacted-", dir=path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as output:
                output.write(after)
            os.chmod(temporary, 0o600)
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
    return count
