"""HardwareOne BLE Secure Channel v1 protocol core.

This module mirrors ``System_BleSecureChannel.cpp`` byte-for-byte while staying
independent of BLE transports.  It deliberately persists nothing: callers own
credential prompting and transport lifecycle, and must bind each notification
callback to the opaque generation returned by :meth:`begin_handshake`.

Only Python's standard library and PyNaCl are used.  PyNaCl supplies the same
libsodium X25519 and ChaCha20-Poly1305-IETF primitives used by the firmware.
"""

from __future__ import annotations

import hashlib
import hmac
import os
import struct
import time
from dataclasses import dataclass
from enum import Enum
from typing import Callable, Optional, Sequence, Tuple, Union

from nacl.bindings import (
    crypto_aead_chacha20poly1305_ietf_decrypt,
    crypto_aead_chacha20poly1305_ietf_encrypt,
    crypto_scalarmult,
    crypto_scalarmult_base,
)
from nacl.exceptions import CryptoError, RuntimeError as SodiumRuntimeError


SC_HELLO = 0x01
SC_HELLO_ACK = 0x02
SC_CONFIRM = 0x03
SC_CONFIRM_ACK = 0x04
SC_REJECT = 0x05
SC_DATA = 0x10

SC_REJECT_NO_PASSPHRASE = 0x01
SC_REJECT_AUTH_FAILED = 0x02

SC_LABEL = b"HW1-SC-v1"
SC_PBKDF2_ITERATIONS = 100_000
SC_KEY_BYTES = 32
SC_NONCE_BYTES = 16
SC_TAG_BYTES = 16

DIR_C2D = 0x00000000
DIR_D2C = 0x00000001

REPLY_VERSION_TEXT = 0x01
REPLY_VERSION_BINARY = 0x02
REPLY_HEADER_BYTES = 5
REPLY_PAYLOAD_BYTES = 195
REPLY_MAX_FRAGMENTS = 255

_UINT16_MAX = (1 << 16) - 1
_UINT64_MAX = (1 << 64) - 1

BytesLike = Union[bytes, bytearray, memoryview]
SecretLike = Union[str, BytesLike]


class SecureChannelError(Exception):
    """Base class for expected protocol failures."""


class StateError(SecureChannelError):
    """An operation was attempted in the wrong handshake/session state."""


class FrameFormatError(SecureChannelError):
    """A wire frame or decrypted reply header is malformed."""


class AuthenticationError(SecureChannelError):
    """X25519 or AEAD authentication failed."""


class RejectError(SecureChannelError):
    """The device returned an unauthenticated handshake REJECT frame."""

    def __init__(self, reason: int):
        self.reason = reason
        if reason == SC_REJECT_NO_PASSPHRASE:
            message = "device has no BLE Secure Channel passphrase"
        elif reason == SC_REJECT_AUTH_FAILED:
            message = "BLE Secure Channel authentication failed"
        else:
            message = f"BLE Secure Channel rejected the handshake (reason {reason})"
        super().__init__(message)


class CounterError(SecureChannelError):
    """A DATA counter violated the per-session sequence contract."""


class CounterGapError(CounterError):
    """A DATA counter skipped one or more expected notifications."""


class ReplayError(CounterError):
    """A DATA counter was duplicated or moved backwards."""


class FragmentError(SecureChannelError):
    """A decrypted five-byte reply-fragment header is inconsistent."""


class DuplicateFragmentError(FragmentError):
    """A fragment index was observed more than once for one message."""


class ConflictingFragmentError(FragmentError):
    """Fragment metadata, payload, order, or message identity conflicts."""


class IncompleteMessageError(FragmentError):
    """A reset, close, timeout, or explicit check found a partial reply."""

    def __init__(self, message_ids: Sequence[int], detail: str = "incomplete reply"):
        self.message_ids = tuple(message_ids)
        suffix = ",".join(str(value) for value in self.message_ids)
        super().__init__(f"{detail}: msgId={suffix}" if suffix else detail)


class StaleSessionError(SecureChannelError):
    """A callback belongs to a reset or replaced Secure Channel generation."""


class ClientState(Enum):
    IDLE = "idle"
    AWAIT_HELLO_ACK = "await_hello_ack"
    AWAIT_CONFIRM_ACK = "await_confirm_ack"
    ESTABLISHED = "established"
    FAILED = "failed"


@dataclass(frozen=True)
class SessionKeys:
    c2d: bytes
    d2c: bytes


@dataclass(frozen=True)
class HelloAck:
    device_public_key: bytes
    device_nonce: bytes


@dataclass(frozen=True)
class ReplyFragment:
    version: int
    message_id: int
    fragment_index: int
    fragment_count: int
    payload: bytes


@dataclass(frozen=True)
class CompletedReply:
    version: int
    message_id: int
    payload: bytes
    fragment_count: int
    first_counter: int
    last_counter: int

    @property
    def is_text(self) -> bool:
        return self.version == REPLY_VERSION_TEXT

    @property
    def is_binary(self) -> bool:
        return self.version == REPLY_VERSION_BINARY


@dataclass
class _PendingReply:
    version: int
    message_id: int
    fragment_count: int
    parts: list[bytes]
    started_at: float
    first_counter: int
    last_counter: int


def _coerce_bytes(value: BytesLike, name: str, length: Optional[int] = None) -> bytes:
    if not isinstance(value, (bytes, bytearray, memoryview)):
        raise TypeError(f"{name} must be bytes-like")
    result = bytes(value)
    if length is not None and len(result) != length:
        raise ValueError(f"{name} must be exactly {length} bytes")
    return result


def _coerce_secret(secret: SecretLike) -> bytes:
    if isinstance(secret, str):
        encoded = secret.encode("utf-8")
    else:
        encoded = _coerce_bytes(secret, "passphrase")
    if not encoded:
        raise ValueError("passphrase must not be empty")
    return encoded


def _require_uint(value: int, maximum: int, name: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or not 0 <= value <= maximum:
        raise ValueError(f"{name} must be an integer in 0..{maximum}")
    return value


def derive_psk(passphrase: SecretLike) -> bytes:
    """Derive the firmware's 32-byte PBKDF2-HMAC-SHA256 PSK."""

    return hashlib.pbkdf2_hmac(
        "sha256",
        _coerce_secret(passphrase),
        SC_LABEL,
        SC_PBKDF2_ITERATIONS,
        dklen=SC_KEY_BYTES,
    )


def hkdf_sha256(ikm: BytesLike, salt: BytesLike, info: BytesLike, length: int) -> bytes:
    """RFC 5869 HKDF-SHA256, restricted to the firmware's two-block limit."""

    ikm_bytes = _coerce_bytes(ikm, "ikm")
    salt_bytes = _coerce_bytes(salt, "salt")
    info_bytes = _coerce_bytes(info, "info")
    if isinstance(length, bool) or not isinstance(length, int) or not 1 <= length <= 64:
        raise ValueError("length must be in 1..64")

    prk = hmac.new(salt_bytes, ikm_bytes, hashlib.sha256).digest()
    output = bytearray()
    previous = b""
    counter = 1
    while len(output) < length:
        previous = hmac.new(
            prk, previous + info_bytes + bytes((counter,)), hashlib.sha256
        ).digest()
        output.extend(previous)
        counter += 1
    return bytes(output[:length])


def x25519_public_key(private_key: BytesLike) -> bytes:
    return crypto_scalarmult_base(_coerce_bytes(private_key, "private_key", SC_KEY_BYTES))


def x25519_shared_secret(private_key: BytesLike, peer_public_key: BytesLike) -> bytes:
    private_bytes = _coerce_bytes(private_key, "private_key", SC_KEY_BYTES)
    public_bytes = _coerce_bytes(peer_public_key, "peer_public_key", SC_KEY_BYTES)
    try:
        return crypto_scalarmult(private_bytes, public_bytes)
    except (CryptoError, SodiumRuntimeError) as exc:
        raise AuthenticationError("invalid X25519 peer public key") from exc


def derive_session_keys(
    app_private_key: BytesLike,
    device_public_key: BytesLike,
    app_nonce: BytesLike,
    device_nonce: BytesLike,
    psk: BytesLike,
) -> SessionKeys:
    """Derive K_c2d || K_d2c exactly as the firmware HELLO handler does."""

    app_nonce_bytes = _coerce_bytes(app_nonce, "app_nonce", SC_NONCE_BYTES)
    device_nonce_bytes = _coerce_bytes(device_nonce, "device_nonce", SC_NONCE_BYTES)
    psk_bytes = _coerce_bytes(psk, "psk", SC_KEY_BYTES)
    shared = x25519_shared_secret(app_private_key, device_public_key)
    expanded = hkdf_sha256(
        shared + psk_bytes,
        app_nonce_bytes + device_nonce_bytes,
        SC_LABEL,
        64,
    )
    return SessionKeys(c2d=expanded[:SC_KEY_BYTES], d2c=expanded[SC_KEY_BYTES:])


def build_hello(app_private_key: BytesLike, app_nonce: BytesLike) -> bytes:
    private_bytes = _coerce_bytes(app_private_key, "app_private_key", SC_KEY_BYTES)
    nonce_bytes = _coerce_bytes(app_nonce, "app_nonce", SC_NONCE_BYTES)
    return bytes((SC_HELLO,)) + x25519_public_key(private_bytes) + nonce_bytes


def parse_hello_ack(frame: BytesLike) -> HelloAck:
    value = _coerce_bytes(frame, "HELLO_ACK")
    if len(value) != 1 + SC_KEY_BYTES + SC_NONCE_BYTES or value[0] != SC_HELLO_ACK:
        raise FrameFormatError("HELLO_ACK must be type 0x02 plus 32-byte key and 16-byte nonce")
    return HelloAck(value[1:33], value[33:49])


def build_hello_ack(device_private_key: BytesLike, device_nonce: BytesLike) -> bytes:
    """Build deterministic device-side fixture bytes; not used by the live client."""

    private_bytes = _coerce_bytes(device_private_key, "device_private_key", SC_KEY_BYTES)
    nonce_bytes = _coerce_bytes(device_nonce, "device_nonce", SC_NONCE_BYTES)
    return bytes((SC_HELLO_ACK,)) + x25519_public_key(private_bytes) + nonce_bytes


def build_nonce(direction: int, counter: int) -> bytes:
    direction_value = _require_uint(direction, (1 << 32) - 1, "direction")
    counter_value = _require_uint(counter, _UINT64_MAX, "counter")
    return struct.pack(">IQ", direction_value, counter_value)


def _encrypt_combined(key: BytesLike, direction: int, counter: int, plaintext: BytesLike) -> bytes:
    key_bytes = _coerce_bytes(key, "key", SC_KEY_BYTES)
    plaintext_bytes = _coerce_bytes(plaintext, "plaintext")
    return crypto_aead_chacha20poly1305_ietf_encrypt(
        plaintext_bytes, None, build_nonce(direction, counter), key_bytes
    )


def _decrypt_combined(key: BytesLike, direction: int, counter: int, combined: BytesLike) -> bytes:
    key_bytes = _coerce_bytes(key, "key", SC_KEY_BYTES)
    combined_bytes = _coerce_bytes(combined, "ciphertext_and_tag")
    if len(combined_bytes) < SC_TAG_BYTES:
        raise FrameFormatError("ciphertext is shorter than the 16-byte tag")
    try:
        return crypto_aead_chacha20poly1305_ietf_decrypt(
            combined_bytes, None, build_nonce(direction, counter), key_bytes
        )
    except CryptoError as exc:
        raise AuthenticationError("ChaCha20-Poly1305 authentication failed") from exc


def build_confirm(keys: SessionKeys) -> bytes:
    return bytes((SC_CONFIRM,)) + _encrypt_combined(keys.c2d, DIR_C2D, 0, b"ok")


def build_confirm_ack(keys: SessionKeys) -> bytes:
    """Build deterministic device-side fixture bytes; not used by the live client."""

    return bytes((SC_CONFIRM_ACK,)) + _encrypt_combined(keys.d2c, DIR_D2C, 0, b"ok")


def verify_confirm_ack(keys: SessionKeys, frame: BytesLike) -> None:
    value = _coerce_bytes(frame, "CONFIRM_ACK")
    if len(value) != 1 + 2 + SC_TAG_BYTES or value[0] != SC_CONFIRM_ACK:
        raise FrameFormatError("CONFIRM_ACK must be exactly 19 bytes with type 0x04")
    plaintext = _decrypt_combined(keys.d2c, DIR_D2C, 0, value[1:])
    if not hmac.compare_digest(plaintext, b"ok"):
        raise AuthenticationError("CONFIRM_ACK plaintext is not 'ok'")


def parse_reject(frame: BytesLike) -> RejectError:
    value = _coerce_bytes(frame, "REJECT")
    if len(value) != 2 or value[0] != SC_REJECT:
        raise FrameFormatError("REJECT must be exactly two bytes with type 0x05")
    return RejectError(value[1])


def build_data_frame(
    key: BytesLike, direction: int, counter: int, plaintext: BytesLike
) -> bytes:
    counter_value = _require_uint(counter, _UINT64_MAX, "counter")
    return (
        bytes((SC_DATA,))
        + struct.pack(">Q", counter_value)
        + _encrypt_combined(key, direction, counter_value, plaintext)
    )


def data_frame_counter(frame: BytesLike) -> int:
    value = _coerce_bytes(frame, "DATA")
    if len(value) < 1 + 8 + SC_TAG_BYTES or value[0] != SC_DATA:
        raise FrameFormatError("DATA must be type 0x10 plus counter, ciphertext, and tag")
    return struct.unpack(">Q", value[1:9])[0]


def decrypt_data_frame(key: BytesLike, direction: int, frame: BytesLike) -> Tuple[int, bytes]:
    value = _coerce_bytes(frame, "DATA")
    counter = data_frame_counter(value)
    plaintext = _decrypt_combined(key, direction, counter, value[9:])
    return counter, plaintext


def build_reply_fragment(
    version: int,
    message_id: int,
    fragment_index: int,
    fragment_count: int,
    payload: BytesLike,
) -> bytes:
    if version not in (REPLY_VERSION_TEXT, REPLY_VERSION_BINARY):
        raise ValueError("version must be text (1) or binary (2)")
    message_value = _require_uint(message_id, _UINT16_MAX, "message_id")
    count_value = _require_uint(fragment_count, REPLY_MAX_FRAGMENTS, "fragment_count")
    index_value = _require_uint(fragment_index, REPLY_MAX_FRAGMENTS - 1, "fragment_index")
    if count_value == 0 or index_value >= count_value:
        raise ValueError("fragment_count must be nonzero and fragment_index must be in range")
    payload_bytes = _coerce_bytes(payload, "payload")
    if len(payload_bytes) > REPLY_PAYLOAD_BYTES:
        raise ValueError(f"reply fragment payload exceeds {REPLY_PAYLOAD_BYTES} bytes")
    return bytes(
        (
            version,
            message_value & 0xFF,
            (message_value >> 8) & 0xFF,
            index_value,
            count_value,
        )
    ) + payload_bytes


def fragment_reply(
    payload: BytesLike, message_id: int, version: int = REPLY_VERSION_TEXT
) -> Tuple[bytes, ...]:
    payload_bytes = _coerce_bytes(payload, "payload")
    fragment_count = max(1, (len(payload_bytes) + REPLY_PAYLOAD_BYTES - 1) // REPLY_PAYLOAD_BYTES)
    if fragment_count > REPLY_MAX_FRAGMENTS:
        raise ValueError("reply exceeds the firmware's 255-fragment limit")
    return tuple(
        build_reply_fragment(
            version,
            message_id,
            index,
            fragment_count,
            payload_bytes[index * REPLY_PAYLOAD_BYTES : (index + 1) * REPLY_PAYLOAD_BYTES],
        )
        for index in range(fragment_count)
    )


def parse_reply_fragment(plaintext: BytesLike) -> ReplyFragment:
    value = _coerce_bytes(plaintext, "reply plaintext")
    if len(value) < REPLY_HEADER_BYTES:
        raise FrameFormatError("decrypted device reply is shorter than its five-byte header")
    version, msg_lo, msg_hi, fragment_index, fragment_count = value[:5]
    if version not in (REPLY_VERSION_TEXT, REPLY_VERSION_BINARY):
        raise FrameFormatError(f"unsupported reply framing version {version}")
    if fragment_count == 0 or fragment_index >= fragment_count:
        raise FrameFormatError("invalid reply fragment index/count")
    if len(value) - REPLY_HEADER_BYTES > REPLY_PAYLOAD_BYTES:
        raise FrameFormatError(
            f"reply fragment payload exceeds {REPLY_PAYLOAD_BYTES} bytes"
        )
    return ReplyFragment(
        version=version,
        message_id=msg_lo | (msg_hi << 8),
        fragment_index=fragment_index,
        fragment_count=fragment_count,
        payload=value[REPLY_HEADER_BYTES:],
    )


class ReplyReassembler:
    """Strict reassembly for the firmware's contiguous device reply messages.

    The firmware holds one TX mutex across every multi-fragment message, so a
    different msgId cannot legally interleave.  DATA counters independently
    enforce delivery order in :class:`SecureChannelV1Client`; this class also
    rejects out-of-order, duplicate, or metadata-changing plaintext fragments.
    """

    def __init__(
        self,
        timeout_seconds: float = 2.0,
        clock: Optional[Callable[[], float]] = None,
    ) -> None:
        if timeout_seconds <= 0:
            raise ValueError("timeout_seconds must be positive")
        self._timeout_seconds = float(timeout_seconds)
        self._clock = clock or time.monotonic
        self._pending: Optional[_PendingReply] = None
        self._expected_message_id: Optional[int] = None

    @property
    def pending_message_ids(self) -> Tuple[int, ...]:
        return () if self._pending is None else (self._pending.message_id,)

    def _now(self, supplied: Optional[float]) -> float:
        return self._clock() if supplied is None else float(supplied)

    def expire(self, now: Optional[float] = None) -> None:
        if self._pending is None:
            return
        current = self._now(now)
        if current - self._pending.started_at >= self._timeout_seconds:
            message_id = self._pending.message_id
            self._pending = None
            raise IncompleteMessageError((message_id,), "reply reassembly timed out")

    def assert_complete(self) -> None:
        if self._pending is not None:
            raise IncompleteMessageError((self._pending.message_id,))

    def reset(self) -> None:
        self._pending = None
        self._expected_message_id = None

    def feed(
        self,
        plaintext: BytesLike,
        counter: int,
        now: Optional[float] = None,
    ) -> Optional[CompletedReply]:
        counter_value = _require_uint(counter, _UINT64_MAX, "counter")
        current = self._now(now)
        self.expire(current)
        fragment = parse_reply_fragment(plaintext)

        if self._pending is None:
            if fragment.fragment_index != 0:
                raise ConflictingFragmentError("a new reply must begin with fragment index 0")
            if (
                self._expected_message_id is not None
                and fragment.message_id != self._expected_message_id
            ):
                raise ConflictingFragmentError(
                    f"expected msgId {self._expected_message_id}, got {fragment.message_id}"
                )
            self._pending = _PendingReply(
                version=fragment.version,
                message_id=fragment.message_id,
                fragment_count=fragment.fragment_count,
                parts=[],
                started_at=current,
                first_counter=counter_value,
                last_counter=counter_value,
            )

        pending = self._pending
        assert pending is not None
        if fragment.message_id != pending.message_id:
            raise ConflictingFragmentError(
                f"msgId {fragment.message_id} interleaved with incomplete msgId {pending.message_id}"
            )
        if (
            fragment.version != pending.version
            or fragment.fragment_count != pending.fragment_count
        ):
            raise ConflictingFragmentError("reply fragment metadata changed within one msgId")

        expected_index = len(pending.parts)
        if fragment.fragment_index < expected_index:
            old_payload = pending.parts[fragment.fragment_index]
            if hmac.compare_digest(old_payload, fragment.payload):
                raise DuplicateFragmentError(
                    f"duplicate fragment {fragment.fragment_index} for msgId {fragment.message_id}"
                )
            raise ConflictingFragmentError(
                f"fragment {fragment.fragment_index} payload changed for msgId {fragment.message_id}"
            )
        if fragment.fragment_index != expected_index:
            raise ConflictingFragmentError(
                f"expected fragment {expected_index}, got {fragment.fragment_index}"
            )

        pending.parts.append(fragment.payload)
        pending.last_counter = counter_value
        if len(pending.parts) != pending.fragment_count:
            return None

        complete = CompletedReply(
            version=pending.version,
            message_id=pending.message_id,
            payload=b"".join(pending.parts),
            fragment_count=pending.fragment_count,
            first_counter=pending.first_counter,
            last_counter=pending.last_counter,
        )
        self._pending = None
        self._expected_message_id = (complete.message_id + 1) & _UINT16_MAX
        return complete


class SecureChannelV1Client:
    """Stateful app-side handshake, DATA counters, and strict reply assembly.

    ``generation`` is an opaque callback fence.  A BLE notification handler
    must capture the generation returned by ``begin_handshake`` and pass that
    same value to every receive method.  ``reset`` advances the generation
    before reporting an incomplete message, so a late old callback can never
    mutate a replacement session.
    """

    def __init__(
        self,
        passphrase: SecretLike,
        *,
        reassembly_timeout: float = 2.0,
        clock: Optional[Callable[[], float]] = None,
    ) -> None:
        self._psk = derive_psk(passphrase)
        self._state = ClientState.IDLE
        self._generation = 0
        self._app_private_key: Optional[bytes] = None
        self._app_nonce: Optional[bytes] = None
        self._keys: Optional[SessionKeys] = None
        self._tx_counter = 1
        self._rx_expected_counter = 1
        self._reassembler = ReplyReassembler(reassembly_timeout, clock)

    @property
    def state(self) -> ClientState:
        return self._state

    @property
    def generation(self) -> int:
        return self._generation

    @property
    def pending_message_ids(self) -> Tuple[int, ...]:
        return self._reassembler.pending_message_ids

    def _check_generation(self, generation: int) -> None:
        if generation != self._generation:
            raise StaleSessionError(
                f"callback generation {generation} does not match active generation {self._generation}"
            )

    def _require_state(self, expected: ClientState) -> None:
        if self._state is not expected:
            raise StateError(f"operation requires {expected.value}, current state is {self._state.value}")

    def _poison(self, error: SecureChannelError) -> None:
        self._app_private_key = None
        self._app_nonce = None
        self._keys = None
        self._reassembler.reset()
        self._state = ClientState.FAILED
        raise error

    def begin_handshake(
        self,
        *,
        app_private_key: Optional[BytesLike] = None,
        app_nonce: Optional[BytesLike] = None,
    ) -> Tuple[int, bytes]:
        self._require_state(ClientState.IDLE)
        private_bytes = (
            os.urandom(SC_KEY_BYTES)
            if app_private_key is None
            else _coerce_bytes(app_private_key, "app_private_key", SC_KEY_BYTES)
        )
        nonce_bytes = (
            os.urandom(SC_NONCE_BYTES)
            if app_nonce is None
            else _coerce_bytes(app_nonce, "app_nonce", SC_NONCE_BYTES)
        )
        self._generation += 1
        self._app_private_key = private_bytes
        self._app_nonce = nonce_bytes
        self._keys = None
        self._tx_counter = 1
        self._rx_expected_counter = 1
        self._reassembler.reset()
        self._state = ClientState.AWAIT_HELLO_ACK
        return self._generation, build_hello(private_bytes, nonce_bytes)

    def receive_hello_ack(self, frame: BytesLike, generation: int) -> bytes:
        self._check_generation(generation)
        self._require_state(ClientState.AWAIT_HELLO_ACK)
        frame_bytes = _coerce_bytes(frame, "HELLO_ACK")
        if frame_bytes[:1] == bytes((SC_REJECT,)):
            self._poison(parse_reject(frame_bytes))
        try:
            ack = parse_hello_ack(frame_bytes)
            assert self._app_private_key is not None and self._app_nonce is not None
            self._keys = derive_session_keys(
                self._app_private_key,
                ack.device_public_key,
                self._app_nonce,
                ack.device_nonce,
                self._psk,
            )
            confirm = build_confirm(self._keys)
        except SecureChannelError as exc:
            self._poison(exc)
        self._state = ClientState.AWAIT_CONFIRM_ACK
        return confirm

    def receive_confirm_ack(self, frame: BytesLike, generation: int) -> None:
        self._check_generation(generation)
        self._require_state(ClientState.AWAIT_CONFIRM_ACK)
        frame_bytes = _coerce_bytes(frame, "CONFIRM_ACK")
        if frame_bytes[:1] == bytes((SC_REJECT,)):
            self._poison(parse_reject(frame_bytes))
        assert self._keys is not None
        try:
            verify_confirm_ack(self._keys, frame_bytes)
        except SecureChannelError as exc:
            self._poison(exc)
        self._app_private_key = None
        self._app_nonce = None
        self._state = ClientState.ESTABLISHED

    def encrypt_command(self, plaintext: BytesLike, generation: int) -> bytes:
        self._check_generation(generation)
        self._require_state(ClientState.ESTABLISHED)
        if self._tx_counter > _UINT64_MAX:
            self._poison(CounterError("c2d counter exhausted"))
        assert self._keys is not None
        frame = build_data_frame(
            self._keys.c2d, DIR_C2D, self._tx_counter, plaintext
        )
        self._tx_counter += 1
        return frame

    def receive_data(
        self,
        frame: BytesLike,
        generation: int,
        *,
        now: Optional[float] = None,
    ) -> Optional[CompletedReply]:
        self._check_generation(generation)
        self._require_state(ClientState.ESTABLISHED)
        assert self._keys is not None
        try:
            counter = data_frame_counter(frame)
            if counter < self._rx_expected_counter:
                raise ReplayError(
                    f"d2c counter {counter} is before expected {self._rx_expected_counter}"
                )
            if counter > self._rx_expected_counter:
                raise CounterGapError(
                    f"d2c counter gap: expected {self._rx_expected_counter}, got {counter}"
                )
            _, plaintext = decrypt_data_frame(self._keys.d2c, DIR_D2C, frame)
            self._rx_expected_counter += 1
            return self._reassembler.feed(plaintext, counter, now)
        except SecureChannelError as exc:
            self._poison(exc)

    def expire_reassembly(self, generation: int, now: Optional[float] = None) -> None:
        self._check_generation(generation)
        self._require_state(ClientState.ESTABLISHED)
        try:
            self._reassembler.expire(now)
        except SecureChannelError as exc:
            self._poison(exc)

    def assert_complete(self, generation: int) -> None:
        self._check_generation(generation)
        self._require_state(ClientState.ESTABLISHED)
        try:
            self._reassembler.assert_complete()
        except SecureChannelError as exc:
            self._poison(exc)

    def reset(self, generation: Optional[int] = None) -> None:
        if generation is not None:
            self._check_generation(generation)
        incomplete = self._reassembler.pending_message_ids
        self._generation += 1
        self._app_private_key = None
        self._app_nonce = None
        self._keys = None
        self._tx_counter = 1
        self._rx_expected_counter = 1
        self._reassembler.reset()
        self._state = ClientState.IDLE
        if incomplete:
            raise IncompleteMessageError(incomplete, "session reset with incomplete reply")


__all__ = [
    "AuthenticationError",
    "ClientState",
    "CompletedReply",
    "ConflictingFragmentError",
    "CounterError",
    "CounterGapError",
    "DIR_C2D",
    "DIR_D2C",
    "DuplicateFragmentError",
    "FrameFormatError",
    "HelloAck",
    "IncompleteMessageError",
    "REPLY_HEADER_BYTES",
    "REPLY_MAX_FRAGMENTS",
    "REPLY_PAYLOAD_BYTES",
    "REPLY_VERSION_BINARY",
    "REPLY_VERSION_TEXT",
    "RejectError",
    "ReplayError",
    "ReplyFragment",
    "ReplyReassembler",
    "SC_CONFIRM",
    "SC_CONFIRM_ACK",
    "SC_DATA",
    "SC_HELLO",
    "SC_HELLO_ACK",
    "SC_LABEL",
    "SC_PBKDF2_ITERATIONS",
    "SC_REJECT",
    "SecureChannelError",
    "SecureChannelV1Client",
    "SessionKeys",
    "StaleSessionError",
    "StateError",
    "build_confirm",
    "build_confirm_ack",
    "build_data_frame",
    "build_hello",
    "build_hello_ack",
    "build_nonce",
    "build_reply_fragment",
    "data_frame_counter",
    "decrypt_data_frame",
    "derive_psk",
    "derive_session_keys",
    "fragment_reply",
    "hkdf_sha256",
    "parse_hello_ack",
    "parse_reject",
    "parse_reply_fragment",
    "verify_confirm_ack",
    "x25519_public_key",
    "x25519_shared_secret",
]
