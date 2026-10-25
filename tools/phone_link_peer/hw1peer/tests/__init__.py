"""Offline tests for hw1peer, run by a tiny runner (no pytest).

Usage, from ``tools/phone_link_peer``::

    .venv/bin/python -m hw1peer.tests                 # every test
    .venv/bin/python -m hw1peer.tests channel x1      # names containing "channel" or "x1"
    .venv/bin/python -m hw1peer.tests -v -x           # list each test, stop at the first failure

Conventions for test modules (any lane may add one):

* a module ``hw1peer/tests/test_<topic>.py``;
* plain functions named ``test_*`` taking no arguments, run in source order;
* ``assert`` for checks; raise :class:`Skip` (or ``unittest.SkipTest``) to skip;
* fixtures through :func:`load_fixture` (the firmware-generated files in
  ``tools/ble_secure/fixtures``; override the folder with ``HW1PEER_FIXTURES``).

Fixture data is synthetic; never put real secrets in a test.
"""

import json
import os
from functools import cache
from pathlib import Path
from typing import Any

# tools/phone_link_peer/hw1peer/tests -> tools
_TOOLS = Path(__file__).resolve().parents[3]
FIXTURES = Path(os.environ.get("HW1PEER_FIXTURES", _TOOLS / "ble_secure" / "fixtures"))


class Skip(Exception):
    """Raise from a test to skip it (with a reason)."""


@cache
def _load(name: str) -> Any:
    with (FIXTURES / name).open(encoding="utf-8") as handle:
        return json.load(handle)


def load_fixture(name: str) -> Any:
    """A parsed fixture file by name (cached; treat the result as read-only)."""
    return _load(name)


def unhex(text: str | None) -> bytes:
    """Bytes from a fixture hex field (None reads as empty)."""
    return bytes.fromhex(text or "")
