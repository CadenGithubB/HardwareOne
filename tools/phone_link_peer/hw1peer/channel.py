"""HW1-PIPE-v1, the outer secure channel of the phone link: PHONE (initiator) side.

Wire-identical to the P4 responder in ``components/hardwareone/BLE_PhoneSecureChannel.h``
(primitives from ``System_BleSecureCrypto.h``) and to the Android app's
``PipeSecureChannel.kt``. The outer channel is unchanged in phone link v2.

Handshake, once per BLE session::

    phone -> HELLO       = 01 || phonePub(32) || phoneNonce(16)                (49 B)
    P4    -> HELLO_ACK   = 02 || devicePub(32) || deviceNonce(16)              (49 B)
    shared = X25519(phonePriv, devicePub); a low-order point or an all-zero
             result is an authentication failure
    K      = HKDF-SHA256(ikm = shared || PSK, salt = phoneNonce || deviceNonce,
                         info = "HW1-PIPE-v1", L = 64)
             kP2D = K[0:32] (phone -> P4), kD2P = K[32:64] (P4 -> phone)
    phone -> CONFIRM     = 03 || ChaCha20-Poly1305(kP2D, nonce(0, 0), "ok")    (19 B)
    P4    -> CONFIRM_ACK = 04 || ChaCha20-Poly1305(kD2P, nonce(1, 0), "ok")    (19 B)
    Until established the P4 may instead send REJECT = 05 || reason (05 02 today).

DATA = ``10 || counter (u64 BE) || ciphertext || tag(16)``: ChaCha20-Poly1305-IETF
without AAD, nonce ``dir (u32 BE) || counter (u64 BE)``, dir 0 = phone -> P4,
1 = P4 -> phone. Sent counters start at 1 and never reach 2^64-1 (that ends the
channel as EXHAUSTED, as the P4's encrypt() refuses it too). A received counter
must exceed the last accepted one (gaps allowed); it is committed only after its
tag verifies.

Every receive-side failure is terminal (Failure.AUTH) and wipes the keys, so
one :class:`PhoneChannel` serves exactly one BLE session. Not thread-safe: the
owner serialises calls. Zeroing is best effort: keys live in ``bytearray``s that
are cleared, but PyNaCl takes ``bytes`` and copies of those may survive.

:class:`DeviceChannel` mirrors the P4 responder for offline tests only.
"""

import hashlib
import hmac
import os
from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum, IntEnum

from nacl import bindings as sodium
from nacl.exceptions import CryptoError

LABEL = "HW1-PIPE-v1"  # hw1phone::kPipeLabel (HKDF info; also the PSK's PBKDF2 salt)
OVERHEAD = 25  # hw1phone::kSecureOverhead: type + counter + tag
HELLO_SIZE = 49
CONFIRM_SIZE = 19
REJECT_SIZE = 2
KEY_SIZE = 32
HANDSHAKE_NONCE_SIZE = 16
TAG_SIZE = 16
DIR_P2D = 0  # nonce direction tag, phone -> P4
DIR_D2P = 1  # nonce direction tag, P4 -> phone
U64_MAX = (1 << 64) - 1
_OK = b"ok"


class MsgType(IntEnum):
    """The first byte of every outer value."""

    HELLO = 0x01
    HELLO_ACK = 0x02
    CONFIRM = 0x03
    CONFIRM_ACK = 0x04
    REJECT = 0x05
    DATA = 0x10


class State(Enum):
    NEW = "new"
    AWAIT_HELLO_ACK = "await_hello_ack"
    AWAIT_CONFIRM_ACK = "await_confirm_ack"
    ESTABLISHED = "established"
    FAILED = "failed"


class Failure(Enum):
    """Why a channel is FAILED."""

    AUTH = "auth"  # any receive-side failure (bad frame, bad tag, low-order key, REJECT, replay)
    EXHAUSTED = "exhausted"  # the TX counter reached 2^64-1


# ---- Outcomes of PhoneChannel.receive -------------------------------------------

@dataclass(frozen=True, slots=True)
class Reply:
    """Send ``value`` (the CONFIRM) to the P4 next."""

    value: bytes


@dataclass(frozen=True, slots=True)
class Established:
    """CONFIRM_ACK verified: DATA may flow both ways."""


@dataclass(frozen=True, slots=True)
class Data:
    """One authenticated DATA payload and its counter."""

    plaintext: bytes
    counter: int


@dataclass(frozen=True, slots=True)
class Rejected:
    """The P4 refused the handshake (REJECT); the channel is FAILED (AUTH)."""

    reason: int


@dataclass(frozen=True, slots=True)
class Invalid:
    """Anything else: the channel is FAILED (AUTH). ``why`` is a short
    diagnostic that never contains key material or plaintext."""

    why: str


type Rx = Reply | Established | Data | Rejected | Invalid


# ---- Primitives -----------------------------------------------------------------

def aead_nonce(direction: int, counter: int) -> bytes:
    """``dir (u32 BE) || counter (u64 BE)``, the 12-byte nonce (hw1sc::scNonce)."""
    return direction.to_bytes(4, "big") + counter.to_bytes(8, "big")


def hkdf_sha256(ikm: bytes, salt: bytes, info: bytes, length: int) -> bytes:
    """RFC 5869 HKDF-SHA256 extract-and-expand (hw1sc::scHkdf)."""
    if not 0 < length <= 255 * 32:
        raise ValueError("HKDF length out of range")
    prk = hmac.digest(salt, ikm, hashlib.sha256)
    out, block, counter = b"", b"", 1
    while len(out) < length:
        block = hmac.digest(prk, block + info + bytes([counter]), hashlib.sha256)
        out += block
        counter += 1
    return out[:length]


def public_key(private_key: bytes) -> bytes:
    """The X25519 public key for a 32-byte private key (libsodium clamps it)."""
    return sodium.crypto_scalarmult_base(bytes(private_key))


def x25519(private_key: bytes, peer_public: bytes) -> bytes | None:
    """X25519(private, peer); None for a low-order point or an all-zero result
    (libsodium's crypto_scalarmult refuses those; the zero check is a backstop)."""
    try:
        shared = sodium.crypto_scalarmult(bytes(private_key), bytes(peer_public))
    except CryptoError:
        return None
    return None if not any(shared) else shared


def derive_keys(shared: bytes, psk: bytes, phone_nonce: bytes, device_nonce: bytes,
                label: str = LABEL) -> tuple[bytes, bytes]:
    """(kP2D, kD2P): HKDF-SHA256(shared || PSK, phoneNonce || deviceNonce, label, 64)."""
    keys = hkdf_sha256(bytes(shared) + bytes(psk), bytes(phone_nonce) + bytes(device_nonce),
                       label.encode("ascii"), 2 * KEY_SIZE)
    return keys[:KEY_SIZE], keys[KEY_SIZE:]


def _encrypt(key: bytes | bytearray, nonce: bytes, plaintext: bytes) -> bytes:
    """ciphertext || tag, no AAD."""
    return sodium.crypto_aead_chacha20poly1305_ietf_encrypt(bytes(plaintext), None, nonce, bytes(key))


def _decrypt(key: bytes | bytearray, nonce: bytes, sealed: bytes) -> bytes | None:
    """The plaintext of ciphertext || tag, or None when the tag fails."""
    if len(sealed) < TAG_SIZE:
        return None
    try:
        return sodium.crypto_aead_chacha20poly1305_ietf_decrypt(bytes(sealed), None, nonce, bytes(key))
    except CryptoError:
        return None


def seal_data(key: bytes | bytearray, direction: int, counter: int, plaintext: bytes) -> bytes:
    """One DATA value under an explicit counter (0..2^64-1). The building block
    of :meth:`PhoneChannel.seal`; tests use it to build frames a channel would
    refuse to send (such as counter 2^64-1)."""
    if not 0 <= counter <= U64_MAX:
        raise ValueError("counter out of the u64 range")
    return (bytes([MsgType.DATA]) + counter.to_bytes(8, "big")
            + _encrypt(key, aead_nonce(direction, counter), plaintext))


def open_data(key: bytes | bytearray, direction: int, value: bytes) -> tuple[int, bytes] | None:
    """(counter, plaintext) of one DATA value, or None when it is malformed or
    fails authentication. No counter policy: callers enforce the increase."""
    value = bytes(value)
    if len(value) < OVERHEAD or value[0] != MsgType.DATA:
        return None
    counter = int.from_bytes(value[1:9], "big")
    plain = _decrypt(key, aead_nonce(direction, counter), value[9:])
    return None if plain is None else (counter, plain)


def _zero(buffer: bytearray) -> None:
    buffer[:] = bytes(len(buffer))


# ---- The phone (initiator) ----------------------------------------------------

class PhoneChannel:
    """The phone side of one HW1-PIPE-v1 session.

    ``psk`` is the 32-byte pipe PSK (``paircode.psk``). For fixture replay pass
    ``private_key`` (32 B) and ``nonce`` (16 B); otherwise ``random`` supplies
    the private key first and then the nonce, as PipeSecureChannel.kt draws
    them. ``label`` changes the HKDF info and exists only to build the
    wrong-domain negative vector; leave it alone.
    """

    def __init__(self, psk: bytes, *, private_key: bytes | None = None, nonce: bytes | None = None,
                 random: Callable[[int], bytes] = os.urandom, label: str = LABEL) -> None:
        if len(psk) != KEY_SIZE:
            raise ValueError(f"PSK must be {KEY_SIZE} bytes")
        if private_key is not None and len(private_key) != KEY_SIZE:
            raise ValueError(f"private key must be {KEY_SIZE} bytes")
        if nonce is not None and len(nonce) != HANDSHAKE_NONCE_SIZE:
            raise ValueError(f"nonce must be {HANDSHAKE_NONCE_SIZE} bytes")
        self._psk = bytearray(psk)
        self._secret = bytearray(private_key) if private_key is not None else bytearray(KEY_SIZE)
        self._injected_key = private_key is not None
        self._phone_nonce = bytearray(nonce) if nonce is not None else bytearray(HANDSHAKE_NONCE_SIZE)
        self._injected_nonce = nonce is not None
        self._random = random
        self._label = label
        self._p2d = bytearray(KEY_SIZE)  # phone -> P4
        self._d2p = bytearray(KEY_SIZE)  # P4 -> phone
        self._tx_counter = 1  # next counter to send; 0 is CONFIRM's
        self._rx_counter = 0  # last accepted; 0 is CONFIRM_ACK's
        self._state = State.NEW
        self._failure: Failure | None = None

    # -- read-only status --
    @property
    def state(self) -> State:
        return self._state

    @property
    def failure(self) -> Failure | None:
        return self._failure

    @property
    def established(self) -> bool:
        return self._state is State.ESTABLISHED

    @property
    def tx_counter(self) -> int:
        """The counter the next sealed DATA will carry."""
        return self._tx_counter

    @property
    def rx_counter(self) -> int:
        """The last accepted P4 DATA counter (0 before the first)."""
        return self._rx_counter

    # -- handshake --
    def hello(self) -> bytes:
        """The HELLO to write first. Only once, on a NEW channel."""
        if self._state is not State.NEW:
            raise RuntimeError("hello() needs a new channel")
        if not self._injected_key:
            self._secret[:] = self._draw(KEY_SIZE)
        if not self._injected_nonce:
            self._phone_nonce[:] = self._draw(HANDSHAKE_NONCE_SIZE)
        self._state = State.AWAIT_HELLO_ACK
        return bytes([MsgType.HELLO]) + public_key(self._secret) + bytes(self._phone_nonce)

    def receive(self, value: bytes) -> Rx:
        """Handle one value from the P4. Never raises for bad input: anything
        unexpected fails the channel and returns Invalid."""
        value = bytes(value)
        match self._state:
            case State.AWAIT_HELLO_ACK:
                return self._rejection(value) or self._hello_ack(value)
            case State.AWAIT_CONFIRM_ACK:
                return self._rejection(value) or self._confirm_ack(value)
            case State.ESTABLISHED:
                return self._data(value)
            case _:
                return self._invalid(f"value in state {self._state.value}")

    # -- data --
    def seal(self, plaintext: bytes) -> bytes | None:
        """One DATA value for ``plaintext`` (``len(plaintext) + OVERHEAD`` bytes),
        or None unless ESTABLISHED. Reaching counter 2^64-1 ends the channel
        (EXHAUSTED) instead of sending it."""
        if self._state is not State.ESTABLISHED:
            return None
        counter = self._tx_counter
        if counter >= U64_MAX:
            self._fail(Failure.EXHAUSTED)
            return None
        self._tx_counter = counter + 1
        return seal_data(self._p2d, DIR_P2D, counter, plaintext)

    def wipe(self) -> None:
        """Zero every key, PSK and private-key buffer and end the channel
        (``failure`` unchanged)."""
        for buffer in (self._psk, self._secret, self._phone_nonce, self._p2d, self._d2p):
            _zero(buffer)
        self._state = State.FAILED

    def set_counters_for_test(self, tx: int, rx: int) -> None:
        """Tests only: the next TX counter and the last accepted RX counter."""
        if not (0 <= tx <= U64_MAX and 0 <= rx <= U64_MAX):
            raise ValueError("counters are u64")
        self._tx_counter, self._rx_counter = tx, rx

    # -- internals --
    def _draw(self, size: int) -> bytes:
        data = self._random(size)
        if len(data) != size:
            raise RuntimeError("random source returned the wrong size")
        return data

    def _rejection(self, value: bytes) -> Rx | None:
        if len(value) != REJECT_SIZE or value[0] != MsgType.REJECT:
            return None
        self._fail(Failure.AUTH)
        return Rejected(value[1])

    def _hello_ack(self, value: bytes) -> Rx:
        if len(value) != HELLO_SIZE or value[0] != MsgType.HELLO_ACK:
            return self._invalid("HELLO_ACK size or type")
        shared = x25519(self._secret, value[1:1 + KEY_SIZE])
        _zero(self._secret)
        if shared is None:
            return self._invalid("low-order or zero X25519 result")
        p2d, d2p = derive_keys(shared, self._psk, self._phone_nonce, value[1 + KEY_SIZE:], self._label)
        self._p2d[:], self._d2p[:] = p2d, d2p
        self._state = State.AWAIT_CONFIRM_ACK
        return Reply(bytes([MsgType.CONFIRM]) + _encrypt(self._p2d, aead_nonce(DIR_P2D, 0), _OK))

    def _confirm_ack(self, value: bytes) -> Rx:
        if len(value) != CONFIRM_SIZE or value[0] != MsgType.CONFIRM_ACK:
            return self._invalid("CONFIRM_ACK size or type")
        if _decrypt(self._d2p, aead_nonce(DIR_D2P, 0), value[1:]) != _OK:
            return self._invalid("CONFIRM_ACK authentication")
        self._state = State.ESTABLISHED
        return Established()

    def _data(self, value: bytes) -> Rx:
        if len(value) < OVERHEAD or value[0] != MsgType.DATA:
            return self._invalid("DATA size or type")
        counter = int.from_bytes(value[1:9], "big")
        if counter <= self._rx_counter:
            return self._invalid("DATA counter not above the last accepted")
        opened = open_data(self._d2p, DIR_D2P, value)
        if opened is None:
            return self._invalid("DATA authentication")
        self._rx_counter = counter  # only after the tag verified
        return Data(opened[1], counter)

    def _invalid(self, why: str) -> Invalid:
        self._fail(Failure.AUTH)
        return Invalid(why)

    def _fail(self, reason: Failure) -> None:
        if self._failure is None:
            self._failure = reason
        self.wipe()


# ---- The P4 (responder), for offline tests ---------------------------------------

class DeviceChannel:
    """A Python mirror of ``hw1phone::SecureChannel`` (the P4 responder), for
    end-to-end tests without a P4. ``receive`` returns ``("reply", bytes)``,
    ``("data", bytes)`` or ``("error", b"")``; after a reply the GATT owner calls
    :meth:`reply_submitted` before the next step, as the firmware does. Errors
    reset the channel (keys erased, phase 0). The P4's REJECT is its GATT
    layer's (``05 02``), not this class's.
    """

    def __init__(self, *, private_key: bytes | None = None, nonce: bytes | None = None,
                 random: Callable[[int], bytes] = os.urandom) -> None:
        self._private_key = private_key
        self._nonce = nonce
        self._random = random
        self._rx_key = bytearray(KEY_SIZE)
        self._tx_key = bytearray(KEY_SIZE)
        self._phase = 0
        self._rx_counter = 0
        self._tx_counter = 1

    @property
    def ready(self) -> bool:
        return self._phase == 4

    def reset(self) -> None:
        _zero(self._rx_key)
        _zero(self._tx_key)
        self._phase, self._rx_counter, self._tx_counter = 0, 0, 1

    def reply_submitted(self) -> bool:
        """The reply was accepted by the local GATT submission."""
        if self._phase in (1, 3):
            self._phase += 1
            return True
        return False

    def receive(self, value: bytes, psk: bytes) -> tuple[str, bytes]:
        value = bytes(value)
        if not value:
            return self._error()
        if value[0] == MsgType.HELLO and self._phase == 0 and len(value) == HELLO_SIZE:
            secret = self._private_key or self._random(KEY_SIZE)
            nonce = self._nonce or self._random(HANDSHAKE_NONCE_SIZE)
            shared = x25519(secret, value[1:1 + KEY_SIZE])
            if shared is None:
                return self._error()
            rx, tx = derive_keys(shared, psk, value[1 + KEY_SIZE:], nonce)
            self._rx_key[:], self._tx_key[:] = rx, tx
            self._phase = 1
            return "reply", bytes([MsgType.HELLO_ACK]) + public_key(secret) + nonce
        if value[0] == MsgType.CONFIRM and self._phase == 2 and len(value) == CONFIRM_SIZE:
            if _decrypt(self._rx_key, aead_nonce(DIR_P2D, 0), value[1:]) != _OK:
                return self._error()
            self._phase = 3
            return "reply", bytes([MsgType.CONFIRM_ACK]) + _encrypt(self._tx_key, aead_nonce(DIR_D2P, 0), _OK)
        if value[0] != MsgType.DATA or not self.ready or len(value) < OVERHEAD:
            return self._error()
        counter = int.from_bytes(value[1:9], "big")
        if counter <= self._rx_counter:
            return self._error()
        opened = open_data(self._rx_key, DIR_P2D, value)
        if opened is None:
            return self._error()
        self._rx_counter = counter
        return "data", opened[1]

    def encrypt(self, plaintext: bytes) -> bytes | None:
        """One P4 DATA value, or None (and a reset) when not ready or exhausted."""
        if not self.ready or self._tx_counter == U64_MAX:
            self.reset()
            return None
        counter = self._tx_counter
        self._tx_counter += 1
        return seal_data(self._tx_key, DIR_D2P, counter, plaintext)

    def _error(self) -> tuple[str, bytes]:
        self.reset()
        return "error", b""
