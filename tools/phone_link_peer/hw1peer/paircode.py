"""Phone link v2 pairing code, passphrase entry and pipe PSK (phone side).

Pairing code: an exact mirror of ``components/hardwareone/BLE_PhonePairCode.h``.

* 16 symbols of ``0123456789ABCDEFGHJKMNPQRSTVWXYZ`` (value = index): 15 data
  symbols and one check symbol over GF(2^5), modulus x^5 + x^2 + 1 (0x25),
  alpha = x::

      check = sum_{i=0..14} alpha^(i+1) * v_i      (GF addition = XOR)

* Canonical form = the 16 uppercase symbols; it is what the P4 stores and what
  PBKDF2 consumes, byte for byte. Display form ``XXXX-XXXX-XXXX-XXXX``.
* Entry is forgiving: outer ASCII whitespace (09-0D, 20) trimmed, ASCII case
  folded, ``-`` and space dropped anywhere, ``I``/``L`` read as 1 and ``O`` as 0.
  Any other byte (``U`` included) means "not a code".

Passphrases: when the entry is not a code, the Android app's pipe passphrase
rule applies (``PipeSecret.kt``; the X3 fixture's "android" column, generated
by ``phone_passphrase_vectors.cpp``): strict UTF-8, no NUL/CR/LF, outer ASCII
whitespace trimmed, 10-128 bytes, ASCII upper, lower, digit and punctuation.
firmware-009 §6.2 #7: a valid code skips the class rules (the P4 does too).

PSK: PBKDF2-HMAC-SHA256(secret bytes, ASCII "HW1-PIPE-v1", 100000, 32), as
``loadPsk()`` in ``BLE_PhonePipe.cpp`` (``hw1phone::kPipeLabel``,
``hw1sc::scPbkdf2``) and ``PipeSecret.derivePsk``.

Secrets are never logged or put in ``repr``. Zeroing is best effort: Python
copies immutable ``bytes`` freely; :class:`PipeSecret` keeps its bytes in a
``bytearray`` that :meth:`PipeSecret.wipe` clears.
"""

import hashlib
import os
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from enum import Enum

# ---- Pairing code (BLE_PhonePairCode.h) --------------------------------------

ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
SYMBOLS = 16
DATA_SYMBOLS = 15
DISPLAY_LEN = 19
MODULUS = 0x25  # x^5 + x^2 + 1, primitive

_ALPHABET_BYTES = ALPHABET.encode("ascii")
# isspace() in the C locale: HT, LF, VT, FF, CR and space (hw1code::outerSpace).
_OUTER_SPACE = frozenset(b" \t\n\v\f\r")
_SEPARATORS = frozenset(b"- ")

type Text = str | bytes | bytearray


def gf_mul_x(a: int) -> int:
    """x * a in GF(2^5); uses the low 5 bits of ``a`` (hw1code::gfMulX)."""
    a = (a & 31) << 1
    return a ^ MODULUS if a & 0x20 else a


def gf_mul(a: int, b: int) -> int:
    """a * b in GF(2^5); uses the low 5 bits of each (hw1code::gfMul)."""
    result, a, b = 0, a & 31, b & 31
    while b:
        if b & 1:
            result ^= a
        a = gf_mul_x(a)
        b >>= 1
    return result


def check_value(values: Sequence[int]) -> int:
    """The check symbol's value (0..31) for 15 data values (hw1code::checkValue)."""
    if len(values) != DATA_SYMBOLS:
        raise ValueError(f"need {DATA_SYMBOLS} data values, got {len(values)}")
    weight, check = 1, 0
    for value in values:
        weight = gf_mul_x(weight)
        check ^= gf_mul(weight, value)
    return check


def value_of(byte: int) -> int:
    """One entry byte's value after case folding and the I/L/O aliases, or -1
    for anything that is not a symbol, separators included (hw1code::valueOf)."""
    if ord("a") <= byte <= ord("z"):
        byte -= ord("a") - ord("A")
    if byte in (ord("I"), ord("L")):
        return 1
    if byte == ord("O"):
        return 0
    return _ALPHABET_BYTES.find(bytes([byte])) if 0 <= byte < 256 else -1


class CodeFault(Enum):
    """Why a text is not a code; the values are the X4 fixture's ``reason``."""

    SYMBOL = "symbol"  # a byte that is neither a symbol, an alias nor a separator
    LENGTH = "length"  # not exactly 16 symbols
    CHECK = "check"  # 16 symbols whose check symbol does not match


def _entry_bytes(text: Text) -> bytes:
    """The bytes the code parser sees. A ``str`` is UTF-8 (lone surrogates pass
    through as non-ASCII bytes, which are never symbols)."""
    if isinstance(text, str):
        return text.encode("utf-8", "surrogatepass")
    if isinstance(text, (bytes, bytearray)):
        return bytes(text)
    raise TypeError(f"expected str or bytes, got {type(text).__name__}")


def _trim(data: bytes) -> bytes:
    """Strip outer C-locale whitespace (Arduino String::trim, hw1code::parse)."""
    start, end = 0, len(data)
    while start < end and data[start] in _OUTER_SPACE:
        start += 1
    while end > start and data[end - 1] in _OUTER_SPACE:
        end -= 1
    return data[start:end]


def _scan(text: Text) -> tuple[list[int], CodeFault | None]:
    """Symbol values of an entry and why it is not a code (None when it is).

    The fault order is the X4 generator's ``reasonOf``: any non-symbol byte is
    SYMBOL, then a count other than 16 is LENGTH, then a wrong check is CHECK.
    It accepts exactly what ``hw1code::parse`` accepts.
    """
    values: list[int] = []
    for byte in _trim(_entry_bytes(text)):
        if byte in _SEPARATORS:
            continue
        value = value_of(byte)
        if value < 0:
            return [], CodeFault.SYMBOL
        values.append(value)
    if len(values) != SYMBOLS:
        return [], CodeFault.LENGTH
    if check_value(values[:DATA_SYMBOLS]) != values[DATA_SYMBOLS]:
        return [], CodeFault.CHECK
    return values, None


def diagnose(text: Text) -> CodeFault | None:
    """Why ``text`` is not a valid code, or None when it is one."""
    return _scan(text)[1]


def parse(text: Text) -> str | None:
    """The canonical 16-symbol form of ``text`` when it is a valid code in any
    accepted spelling, else None (hw1code::parse)."""
    values, fault = _scan(text)
    return None if fault else "".join(ALPHABET[v] for v in values)


# The contract names both; they are the same operation.
normalise = parse


def is_code(text: Text) -> bool:
    """True when ``text`` is a valid code in any accepted spelling (hw1code::isCode)."""
    return diagnose(text) is None


def from_values(values: Sequence[int]) -> str:
    """The canonical code for 15 data values (low 5 bits used) plus their check
    (hw1code::fromValues)."""
    data = [v & 31 for v in values]
    return "".join(ALPHABET[v] for v in data) + ALPHABET[check_value(data)]


def from_random(words: Sequence[int]) -> str:
    """The P4's generation from three 32-bit random words: value i = bits
    5*(i%6).. of word i//6 (30 bits of words 0 and 1, 15 of word 2), then the
    check (hw1code::fromRandom)."""
    if len(words) != 3:
        raise ValueError("need three 32-bit words")
    return from_values([(words[i // 6] >> (5 * (i % 6))) & 31 for i in range(DATA_SYMBOLS)])


def generate(random: Callable[[int], bytes] = os.urandom) -> str:
    """A fresh code from 12 random bytes, as the P4's ``phone pair new`` makes
    one (for tests and local experiments; the phone never generates codes)."""
    raw = random(12)
    return from_random([int.from_bytes(raw[i:i + 4], "little") for i in (0, 4, 8)])


def display(canonical: str) -> str:
    """``XXXX-XXXX-XXXX-XXXX`` from the 16 canonical symbols (hw1code::display).

    Raises ValueError for anything that is not 16 alphabet symbols; parse()
    first to accept other spellings.
    """
    if len(canonical) != SYMBOLS or any(c not in ALPHABET for c in canonical):
        raise ValueError("display() needs the 16 canonical symbols")
    return "-".join(canonical[i:i + 4] for i in range(0, SYMBOLS, 4))


# ---- PSK (BLE_PhonePipe.cpp loadPsk, System_BleSecureCrypto.h scPbkdf2) ----------

PIPE_LABEL = "HW1-PIPE-v1"  # hw1phone::kPipeLabel: PBKDF2 salt and HKDF info
PBKDF2_ITERATIONS = 100_000
PSK_BYTES = 32


def pbkdf2_sha256(secret: bytes | bytearray, salt: bytes, iterations: int = PBKDF2_ITERATIONS) -> bytes:
    """One 32-byte PBKDF2-HMAC-SHA256 block (RFC 2898), as hw1sc::scPbkdf2."""
    if iterations < 1:
        raise ValueError("iterations must be at least 1")
    return hashlib.pbkdf2_hmac("sha256", secret, salt, iterations, PSK_BYTES)


def psk(secret: bytes | bytearray) -> bytes:
    """The pipe PSK for the stored secret bytes: PBKDF2-HMAC-SHA256(secret,
    "HW1-PIPE-v1", 100000, 32). Pass a code's canonical ASCII or a passphrase's
    exact stored bytes. Slow by design (about 0.05-0.1 s)."""
    if not isinstance(secret, (bytes, bytearray)):
        raise TypeError("psk() takes the secret's bytes")
    return pbkdf2_sha256(secret, PIPE_LABEL.encode("ascii"))


# ---- Secret entry: a code, or a passphrase the app would accept ---------------

PASSPHRASE_MIN_BYTES = 10
PASSPHRASE_MAX_BYTES = 128


class EntryFault(Enum):
    """Why an entry is refused; the values are the X3 fixture's ``android_reason``."""

    INVALID_UTF8 = "not valid UTF-8 text"
    CONTROL = "contains NUL, CR or LF"
    TOO_SHORT = "fewer than 10 bytes after trimming"
    TOO_LONG = "more than 128 bytes after trimming"
    NO_UPPER = "no ASCII uppercase letter"
    NO_LOWER = "no ASCII lowercase letter"
    NO_DIGIT = "no ASCII digit"
    NO_PUNCT = "no ASCII punctuation"


class SecretKind(Enum):
    CODE = "code"  # a pairing code: canonical symbols; the only kind that may carry the console
    PASSPHRASE = "passphrase"  # relay only


@dataclass(eq=False)
class PipeSecret:
    """An accepted entry: the exact bytes the P4 stores and PBKDF2 consumes.

    ``repr`` never shows the bytes. Call :meth:`wipe` when done.
    """

    kind: SecretKind
    secret: bytearray = field(repr=False)

    @property
    def is_code(self) -> bool:
        """True for a pairing code (firmware-009 §6.2 #7: refuse the console
        with 16 whenever this is False)."""
        return self.kind is SecretKind.CODE

    def psk(self) -> bytes:
        """PBKDF2 of the stored bytes (see :func:`psk`)."""
        return psk(self.secret)

    def wipe(self) -> None:
        """Zero the held bytes (best effort: earlier copies may survive)."""
        self.secret[:] = bytes(len(self.secret))


@dataclass(frozen=True)
class EntryRefused:
    """A refused entry. Carries the reason only, never the input."""

    fault: EntryFault


def _is_ascii_punct(byte: int) -> bool:
    """ispunct() in the C locale: 21-2F, 3A-40, 5B-60, 7B-7E."""
    return 0x21 <= byte <= 0x2F or 0x3A <= byte <= 0x40 or 0x5B <= byte <= 0x60 or 0x7B <= byte <= 0x7E


def passphrase_bytes(entry: Text) -> bytes | EntryFault:
    """The app's passphrase rule alone (no code branch): the trimmed UTF-8 bytes
    to store, or why the entry is refused.

    Order as the X3 generator's ``androidRule``: strict UTF-8, no NUL/CR/LF,
    trim outer ASCII 09-0D/20, at least 10 and at most 128 bytes, then ASCII
    upper, lower, digit and punctuation. Bytes 0x80 and above count toward the
    length and belong to no class. (PipeSecret.kt checks NUL/CR/LF before
    encoding; the two orders accept and refuse the same entries.)
    """
    if isinstance(entry, str):
        try:
            data = entry.encode("utf-8")  # strict: a lone surrogate is malformed
        except UnicodeEncodeError:
            return EntryFault.INVALID_UTF8
    else:
        data = _entry_bytes(entry)
        try:
            data.decode("utf-8")
        except UnicodeDecodeError:
            return EntryFault.INVALID_UTF8
    if any(b in (0x00, 0x0D, 0x0A) for b in data):
        return EntryFault.CONTROL
    trimmed = _trim(data)
    if len(trimmed) < PASSPHRASE_MIN_BYTES:
        return EntryFault.TOO_SHORT
    if len(trimmed) > PASSPHRASE_MAX_BYTES:
        return EntryFault.TOO_LONG
    if not any(0x41 <= b <= 0x5A for b in trimmed):
        return EntryFault.NO_UPPER
    if not any(0x61 <= b <= 0x7A for b in trimmed):
        return EntryFault.NO_LOWER
    if not any(0x30 <= b <= 0x39 for b in trimmed):
        return EntryFault.NO_DIGIT
    if not any(_is_ascii_punct(b) for b in trimmed):
        return EntryFault.NO_PUNCT
    return trimmed


def pipe_secret(entry: Text) -> PipeSecret | EntryRefused:
    """What the peer stores for a typed entry.

    A valid code in any accepted spelling becomes its canonical ASCII (kind
    CODE); the class rules do not apply to it. Anything else must pass the
    app's passphrase rule (kind PASSPHRASE, the trimmed UTF-8 bytes).
    """
    canonical = parse(entry)
    if canonical is not None:
        return PipeSecret(SecretKind.CODE, bytearray(canonical.encode("ascii")))
    result = passphrase_bytes(entry)
    if isinstance(result, EntryFault):
        return EntryRefused(result)
    return PipeSecret(SecretKind.PASSPHRASE, bytearray(result))
