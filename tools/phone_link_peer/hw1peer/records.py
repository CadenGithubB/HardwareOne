"""Phone link v2 console records: the byte layer inside the console stream (pipe slot 2).

A Python mirror of the firmware's ``BLE_PhonePipeConsoleCore.h`` for the PHONE role, so this
peer and the P4 agree byte for byte. Firmware-009 §5.2 is the wire contract; where it and the
header or the X4 fixtures (``phone_console_v2_records.json``) differ, the header and fixtures win.

Record = ``type u8 | flags u8 (bit0 MORE, bits 1-7 zero) | len u16 LE | payload``.

* P4 -> phone: INFO 0x01 (the first record, once), TEXT 0x02 / BLOB 0x03 (one message per MORE
  chain, at most 4608 B), CARD 0x04, MIRROR 0x05, ABORT 0x06 (cuts the open chain); TICKET 0x07 is
  reserved (P3).
* Phone -> P4: COMMAND 0x81 (one CLI line), BINARY 0x82 (one OtaBleWire frame,
  ``00 'HW1OTA' 01 offset_be32 chunk``); RESUME 0x83 is reserved (P3).

Layers, bottom up:

1. Header codec, type table and bounds: :func:`encode_header`, :func:`parse_header`,
   :func:`encode`, :func:`type_spec`, :func:`header_fault`, :func:`scan`.
2. :class:`PhoneReceiver`: the firmware's phone-side validator, one verdict per record (INFO
   first and once, chains, the 4608 B cap, ABORT, skipped types, direction, flags).
3. :class:`RecordAssembler`: a byte stream (any DATA split) into fixture-level deliveries
   (``info``/``text``/``blob``/``card``/``mirror``/``abort``) or one :class:`Malformed`.
4. :class:`ConsoleEndpoint`: deliveries into application events (:class:`Info`, :class:`Text`,
   :class:`Blob` paired with its ``enc:"raw"`` TEXT header, :class:`Card`, :class:`Mirror`,
   :class:`Abort`, :class:`Malformed`), plus the COMMAND and BINARY builders.

A P4-side writer (:class:`Egress`, :func:`build_info`, :class:`P4Writer`) is included for fakes
and fixture replay; the peer itself never sends P4 records.

Pure: no I/O, no logging, no BLE. Nothing here prints payloads, so console secrets (a ``login``
line, say) never reach a log from this module.
"""

import json
from dataclasses import dataclass, replace
from enum import Enum, IntEnum
from typing import Final

type ByteSource = bytes | bytearray | memoryview


# --------------------------------------------------------------------------------------------
# Types and limits (BLE_PhonePipeConsoleCore.h constants)
# --------------------------------------------------------------------------------------------

class RecordType(IntEnum):
    """The record types the contract defines. Any other byte value is an unknown type."""

    INFO = 0x01
    TEXT = 0x02
    BLOB = 0x03
    CARD = 0x04
    MIRROR = 0x05
    ABORT = 0x06
    TICKET = 0x07
    COMMAND = 0x81
    BINARY = 0x82
    RESUME = 0x83


MORE: Final = 0x01                     # flags bit0; bits 1-7 must be zero
HEADER_SIZE: Final = 4
PAYLOAD_MAX: Final = 1024
RECORD_MAX: Final = HEADER_SIZE + PAYLOAD_MAX
INFO_MAX: Final = 256
TEXT_MAX: Final = 1024                 # one TEXT or BLOB record
CHAIN_MAX: Final = 4608                # one TEXT or BLOB chain (CMD_RESULT_MAX 4096 + 512)
CARD_MAX: Final = 512
MIRROR_MAX: Final = 512
COMMAND_MAX: Final = 511
BINARY_MIN: Final = 13
BINARY_MAX: Final = 489
UNKNOWN_MAX: Final = 1024              # unknown and reserved types: 0..1024, no MORE
OTA_MAGIC: Final = b"\x00HW1OTA\x01"   # every BINARY payload starts with it
OTA_HEAD_SIZE: Final = len(OTA_MAGIC) + 4          # magic + offset u32 BE = 12
OTA_CHUNK_MAX: Final = BINARY_MAX - OTA_HEAD_SIZE  # 477
SCAN_PEEK: Final = HEADER_SIZE + len(OTA_MAGIC)    # scan() never needs more than this
INFO_VERSION: Final = 1                # INFO "v": the record format, not the inner version
INFO_FIELD: Final = 64                 # escaped bytes per INFO string
DEFAULT_MODEL: Final = "HardwareOne"   # INFO model when the board name is empty
STREAM_RESET_MALFORMED: Final = 1      # the stream RESET code the phone answers malformed with

assert BINARY_MIN == len(OTA_MAGIC) + 4 + 1 and BINARY_MAX + HEADER_SIZE == 493
assert CHAIN_MAX == 4096 + 512


class Side(Enum):
    """The receiving side a rule is applied at."""

    P4 = "p4"
    PHONE = "phone"


class Direction(IntEnum):
    """A type's direction in the type table (``TypeSpec::dir``)."""

    UNKNOWN = 0
    TO_PHONE = 1
    TO_P4 = 2


class Verdict(Enum):
    """What a receiver does with the record at the head of its stream (``verdictName``)."""

    NEED_MORE = "need-more"
    DELIVER = "deliver"
    SKIP = "skip"            # unknown or reserved type: consumed and counted, nothing delivered
    IGNORE = "ignore"        # a stray ABORT on the phone
    MALFORMED = "malformed"  # the stream is dead: the phone answers with stream RESET 1


class Fault(Enum):
    """Why a record is malformed (``faultName``)."""

    NONE = "none"
    FLAGS = "flags"              # a flag bit 1-7 set
    MORE = "more"                # MORE on a type other than TEXT/BLOB
    LENGTH = "length"            # len outside the type's bounds
    DIRECTION = "direction"      # a known type sent the wrong way
    MAGIC = "magic"              # a BINARY without the OTA magic (P4 side)
    TYPE = "type"                # the P4 writer asked for a type it never writes
    INTERLEAVE = "interleave"    # another type inside an open chain
    CAP = "cap"                  # the chain passes 4608, or reaches it with MORE
    STRAY_ABORT = "stray-abort"  # the P4 writer asked for ABORT with no chain open
    INFO_ORDER = "info-order"    # a first record that is not INFO, or a second INFO


_TYPE_NAMES: Final = {t.value: t.name for t in RecordType}


def type_name(code: int) -> str:
    """The fixture/firmware name of a type code: ``INFO`` .. ``RESUME``, else ``UNKNOWN``."""
    return _TYPE_NAMES.get(code, "UNKNOWN")


class RecordError(ValueError):
    """A record the builder refuses to make (out of bounds, a newline, no OTA magic, ...)."""


# --------------------------------------------------------------------------------------------
# Header codec
# --------------------------------------------------------------------------------------------

@dataclass(frozen=True, slots=True)
class Header:
    """A parsed 4-byte record header."""

    type: int
    flags: int
    length: int

    @property
    def more(self) -> bool:
        return bool(self.flags & MORE)

    @property
    def name(self) -> str:
        return type_name(self.type)

    @property
    def size(self) -> int:
        """The whole record: header plus payload."""
        return HEADER_SIZE + self.length

    def encode(self) -> bytes:
        return encode_header(self.type, self.flags, self.length)


def encode_header(rtype: int, flags: int, length: int) -> bytes:
    """``type | flags | len u16 LE``. Raises :class:`RecordError` when a field does not fit."""
    if not (0 <= rtype <= 0xFF and 0 <= flags <= 0xFF and 0 <= length <= 0xFFFF):
        raise RecordError(f"header field out of range: type={rtype} flags={flags} len={length}")
    return bytes((rtype, flags, length & 0xFF, length >> 8))


def parse_header(data: ByteSource) -> Header:
    """Parse the first 4 bytes. Raises :class:`RecordError` when fewer are there."""
    if len(data) < HEADER_SIZE:
        raise RecordError(f"a header needs {HEADER_SIZE} bytes, got {len(data)}")
    return Header(data[0], data[1], data[2] | data[3] << 8)


def encode(rtype: int, flags: int, payload: ByteSource = b"") -> bytes:
    """Header plus payload, with no bounds check (``hw1rec::encode``): for builders and tests.

    Raises :class:`RecordError` only when the payload is longer than a u16 can say.
    """
    return encode_header(rtype, flags, len(payload)) + bytes(payload)


# --------------------------------------------------------------------------------------------
# Type table and stateless rules
# --------------------------------------------------------------------------------------------

@dataclass(frozen=True, slots=True)
class TypeSpec:
    """What a type is: its direction, whether it is reserved, MORE allowed, length bounds."""

    direction: Direction
    reserved: bool
    more: bool
    min_len: int
    max_len: int


_SPECS: Final = {
    RecordType.INFO: TypeSpec(Direction.TO_PHONE, False, False, 1, INFO_MAX),
    RecordType.TEXT: TypeSpec(Direction.TO_PHONE, False, True, 1, TEXT_MAX),
    RecordType.BLOB: TypeSpec(Direction.TO_PHONE, False, True, 1, TEXT_MAX),
    RecordType.CARD: TypeSpec(Direction.TO_PHONE, False, False, 1, CARD_MAX),
    RecordType.MIRROR: TypeSpec(Direction.TO_PHONE, False, False, 1, MIRROR_MAX),
    RecordType.ABORT: TypeSpec(Direction.TO_PHONE, False, False, 0, 0),
    RecordType.TICKET: TypeSpec(Direction.TO_PHONE, True, False, 0, UNKNOWN_MAX),
    RecordType.COMMAND: TypeSpec(Direction.TO_P4, False, False, 1, COMMAND_MAX),
    RecordType.BINARY: TypeSpec(Direction.TO_P4, False, False, BINARY_MIN, BINARY_MAX),
    RecordType.RESUME: TypeSpec(Direction.TO_P4, True, False, 0, UNKNOWN_MAX),
}
_UNKNOWN_SPEC: Final = TypeSpec(Direction.UNKNOWN, False, False, 0, UNKNOWN_MAX)


def type_spec(code: int) -> TypeSpec:
    """The type table row for ``code``; reserved types carry the unknown bounds."""
    return _SPECS.get(code, _UNKNOWN_SPEC)


def _arriving(at: Side) -> Direction:
    """The direction of the records a side receives."""
    return Direction.TO_P4 if at is Side.P4 else Direction.TO_PHONE


def is_skipped(at: Side, code: int) -> bool:
    """A record of this type is skipped (consumed, counted, not delivered) when it reaches ``at``:
    an unknown type, or the reserved type addressed to that side (TICKET at the phone, RESUME at
    the P4)."""
    spec = type_spec(code)
    return spec.direction is Direction.UNKNOWN or (spec.reserved and spec.direction is _arriving(at))


def header_fault(at: Side, header: Header) -> Fault:
    """The stateless header rules at the receiving side, in the firmware's order."""
    if header.flags & ~MORE & 0xFF:
        return Fault.FLAGS
    spec = type_spec(header.type)
    if spec.direction is not Direction.UNKNOWN and spec.direction is not _arriving(at):
        return Fault.DIRECTION
    if header.more and not spec.more:
        return Fault.MORE
    if not spec.min_len <= header.length <= spec.max_len:
        return Fault.LENGTH
    return Fault.NONE


@dataclass(frozen=True, slots=True)
class Scan:
    """One receiver verdict on the record at the head of a stream (``hw1rec::Scan``)."""

    verdict: Verdict = Verdict.NEED_MORE
    fault: Fault = Fault.NONE
    header: Header | None = None   # None until 4 bytes are there
    size: int = HEADER_SIZE         # the record's bytes (4 + len); 4 before the header
    chain_end: bool = False         # PhoneReceiver: this record ended a chain (last record or ABORT)
    chain_total: int = 0            # PhoneReceiver: the chain's payload bytes so far / at its end


def scan(at: Side, data: ByteSource) -> Scan:
    """One record at the start of ``data`` (all bytes received so far), stateless.

    Malformed shows as soon as the offending byte is there (header, or a BINARY's magic at the
    P4); Deliver/Skip need the whole record. The verdict for a growing prefix never goes back.
    Only the first :data:`SCAN_PEEK` bytes are read.
    """
    have = len(data)
    if have < HEADER_SIZE:
        return Scan()
    header = parse_header(data)
    fault = header_fault(at, header)
    if fault is Fault.NONE and at is Side.P4 and header.type == RecordType.BINARY:
        n = min(have - HEADER_SIZE, len(OTA_MAGIC))
        if bytes(data[HEADER_SIZE:HEADER_SIZE + n]) != OTA_MAGIC[:n]:
            fault = Fault.MAGIC
    if fault is not Fault.NONE:
        verdict = Verdict.MALFORMED
    elif have >= header.size:
        verdict = Verdict.SKIP if is_skipped(at, header.type) else Verdict.DELIVER
    else:
        verdict = Verdict.NEED_MORE
    return Scan(verdict, fault, header, header.size)


def scan_ingress(data: ByteSource) -> Scan:
    """The P4's ingress verdict on the phone's records (``hw1rec::scanIngress``)."""
    return scan(Side.P4, data)


# --------------------------------------------------------------------------------------------
# The phone's receiver validator (hw1rec::PhoneReceiver)
# --------------------------------------------------------------------------------------------

class PhoneReceiver:
    """The phone's view of the P4's records: :func:`scan` at the phone plus INFO first and once,
    the chain rules, and a stray ABORT ignored.

    State advances only on a whole record (Deliver/Ignore); Skip leaves it, so an unknown type or
    TICKET is skipped even inside an open chain; Malformed latches (every later call says
    Malformed with the first fault). Calling :meth:`next` again on a longer prefix of the same
    record is safe: NeedMore changes nothing.
    """

    __slots__ = ("_info", "_type", "_total", "_failed")

    def __init__(self) -> None:
        self._info = False
        self._type = 0          # the open chain's type, 0 when none
        self._total = 0         # its payload bytes so far
        self._failed = Fault.NONE

    def reset(self) -> None:
        self.__init__()

    def copy(self) -> "PhoneReceiver":
        twin = PhoneReceiver()
        twin._info, twin._type, twin._total, twin._failed = self._info, self._type, self._total, self._failed
        return twin

    @property
    def failed(self) -> bool:
        return self._failed is not Fault.NONE

    @property
    def fault(self) -> Fault:
        return self._failed

    @property
    def info_seen(self) -> bool:
        return self._info

    @property
    def chain_open(self) -> bool:
        return self._type != 0

    @property
    def chain_type(self) -> int:
        return self._type

    @property
    def chain_total(self) -> int:
        return self._total

    def next(self, data: ByteSource) -> Scan:
        """The verdict on the record at the start of ``data``; advances on Deliver/Ignore only.
        The caller consumes ``size`` bytes on Deliver, Skip and Ignore."""
        r = scan(Side.PHONE, data)
        if self.failed:
            return replace(r, verdict=Verdict.MALFORMED, fault=self._failed)
        if r.verdict is Verdict.MALFORMED:
            self._failed = r.fault
            return r
        if r.header is None or is_skipped(Side.PHONE, r.header.type):
            return r
        fault = self._state_fault(r.header)
        if fault is not Fault.NONE:
            self._failed = fault
            return replace(r, verdict=Verdict.MALFORMED, fault=fault)
        if r.verdict is Verdict.NEED_MORE:
            return r
        return self._advance(r)

    def _state_fault(self, h: Header) -> Fault:
        if h.type == RecordType.INFO:
            return Fault.INFO_ORDER if self._info else Fault.NONE
        if not self._info:
            return Fault.INFO_ORDER
        if not self.chain_open or h.type == RecordType.ABORT:
            return Fault.NONE
        if h.type != self._type:
            return Fault.INTERLEAVE
        total = self._total + h.length
        return Fault.CAP if total > CHAIN_MAX or (h.more and total >= CHAIN_MAX) else Fault.NONE

    def _advance(self, r: Scan) -> Scan:
        h = r.header
        assert h is not None
        if h.type == RecordType.INFO:
            self._info = True
        elif h.type == RecordType.ABORT:
            if not self.chain_open:
                return replace(r, verdict=Verdict.IGNORE)
            r = replace(r, chain_end=True, chain_total=self._total)
            self._type, self._total = 0, 0
        elif h.type in (RecordType.TEXT, RecordType.BLOB):
            self._total = (self._total if self.chain_open else 0) + h.length
            r = replace(r, chain_total=self._total)
            if h.more:
                self._type = h.type
            else:
                r = replace(r, chain_end=True)
                self._type, self._total = 0, 0
        return r


# --------------------------------------------------------------------------------------------
# Phone -> P4 builders
# --------------------------------------------------------------------------------------------

def _as_bytes(value: str | ByteSource) -> bytes:
    return value.encode("utf-8") if isinstance(value, str) else bytes(value)


def build_command(line: str | ByteSource) -> bytes:
    """One COMMAND record: one CLI line, 1..511 bytes (a ``str`` is sent as UTF-8), no newline.

    Other bytes go through unchanged (the P4 drops NUL and maps CR/LF/TAB to space itself).
    Raises :class:`RecordError` for an empty or overlong line or one holding LF.
    """
    payload = _as_bytes(line)
    if not 1 <= len(payload) <= COMMAND_MAX:
        raise RecordError(f"a COMMAND is 1..{COMMAND_MAX} bytes, got {len(payload)}")
    if b"\n" in payload:
        raise RecordError("a COMMAND is one line: no newline")
    return encode(RecordType.COMMAND, 0, payload)


def ota_frame(offset: int, chunk: ByteSource) -> bytes:
    """One OtaBleWire frame: ``00 'HW1OTA' 01 | offset u32 BE | chunk`` (chunk 1..477 bytes)."""
    if not 0 <= offset <= 0xFFFFFFFF:
        raise RecordError(f"OTA offset out of range: {offset}")
    if not 1 <= len(chunk) <= OTA_CHUNK_MAX:
        raise RecordError(f"an OTA chunk is 1..{OTA_CHUNK_MAX} bytes, got {len(chunk)}")
    return OTA_MAGIC + offset.to_bytes(4, "big") + bytes(chunk)


def build_binary(frame: ByteSource) -> bytes:
    """One BINARY record carrying one OtaBleWire frame: 13..489 bytes starting with the magic.

    Raises :class:`RecordError` when the frame is out of bounds or lacks the magic.
    """
    payload = bytes(frame)
    if not BINARY_MIN <= len(payload) <= BINARY_MAX:
        raise RecordError(f"a BINARY is {BINARY_MIN}..{BINARY_MAX} bytes, got {len(payload)}")
    if not payload.startswith(OTA_MAGIC):
        raise RecordError("a BINARY must start with 00 'HW1OTA' 01")
    return encode(RecordType.BINARY, 0, payload)


# --------------------------------------------------------------------------------------------
# P4 side, for fakes and fixture replay (hw1rec::Egress, sliceLen/sliceFlags, buildInfo)
# --------------------------------------------------------------------------------------------

_P4_WRITES: Final = frozenset((RecordType.TEXT, RecordType.BLOB, RecordType.CARD, RecordType.MIRROR,
                               RecordType.ABORT))


class Egress:
    """The P4 writer's chain state on one console stream: which record may go next.

    :meth:`check` never changes state; :meth:`commit` applies a record that passed. The P4 writes
    TEXT, BLOB, CARD, MIRROR and ABORT this way; INFO, TICKET and anything else are Fault.TYPE.
    """

    __slots__ = ("_type", "_total")

    def __init__(self) -> None:
        self._type = 0
        self._total = 0

    def reset(self) -> None:
        self._type, self._total = 0, 0

    @property
    def chain_open(self) -> bool:
        return self._type != 0

    @property
    def chain_type(self) -> int:
        return self._type

    @property
    def chain_total(self) -> int:
        return self._total

    def check(self, rtype: int, flags: int, length: int) -> Fault:
        if flags & ~MORE & 0xFF:
            return Fault.FLAGS
        if rtype not in _P4_WRITES:
            return Fault.TYPE
        spec = type_spec(rtype)
        if flags & MORE and not spec.more:
            return Fault.MORE
        if not spec.min_len <= length <= spec.max_len:
            return Fault.LENGTH
        if rtype == RecordType.ABORT:
            return Fault.NONE if self.chain_open else Fault.STRAY_ABORT
        if not self.chain_open:
            return Fault.NONE
        if rtype != self._type:
            return Fault.INTERLEAVE
        total = self._total + length
        return Fault.CAP if total > CHAIN_MAX or (flags & MORE and total >= CHAIN_MAX) else Fault.NONE

    def commit(self, rtype: int, flags: int, length: int) -> bool:
        if self.check(rtype, flags, length) is not Fault.NONE:
            return False
        if rtype == RecordType.ABORT:
            self.reset()
        elif rtype in (RecordType.TEXT, RecordType.BLOB):
            if flags & MORE:
                self._total = (self._total if self.chain_open else 0) + length
                self._type = rtype
            else:
                self.reset()
        return True


def slice_len(remaining: int) -> int:
    """The next chain record's length for a writer with ``remaining`` (>= 1) bytes left."""
    return min(remaining, TEXT_MAX)


def slice_flags(remaining: int) -> int:
    """The next chain record's flags: MORE while more than one record's worth remains."""
    return MORE if remaining > TEXT_MAX else 0


def chain_fits(total: int) -> bool:
    """A whole chain is 1..4608 bytes."""
    return 1 <= total <= CHAIN_MAX


def _c_string(value: str | ByteSource | None) -> bytes:
    """``value`` as the firmware's C string sees it: None is empty, and it ends at the first NUL."""
    return b"" if value is None else _as_bytes(value).split(b"\x00", 1)[0]


def _info_field(raw: bytes) -> str:
    """One INFO string, JSON-escaped as the firmware does it.

    ``"`` and ``\\`` escaped, control bytes and DEL as ``\\u00XX``, bytes >= 0x80 as ``?``; cut at
    64 escaped bytes without splitting an escape.
    """
    out: list[str] = []
    used = 0
    for byte in raw:
        if byte in (0x22, 0x5C):
            piece = "\\" + chr(byte)
        elif byte < 0x20 or byte == 0x7F:
            piece = f"\\u{byte:04x}"
        else:
            piece = "?" if byte >= 0x80 else chr(byte)
        if used + len(piece) > INFO_FIELD:
            break
        out.append(piece)
        used += len(piece)
    return "".join(out)


def build_info(fw: str | ByteSource | None, model: str | ByteSource | None) -> bytes:
    """The INFO payload the P4 writes (``hw1rec::buildInfo``), compact JSON in this key order:
    ``{"v":1,"fw":"<fw>","model":"<model>","line":511,"bin":489,"auth":true,"resume":false}``.
    An empty or missing model is ``HardwareOne``."""
    model_bytes = _c_string(model) or DEFAULT_MODEL.encode("ascii")
    text = (f'{{"v":{INFO_VERSION},"fw":"{_info_field(_c_string(fw))}","model":"{_info_field(model_bytes)}",'
            f'"line":{COMMAND_MAX},"bin":{BINARY_MAX},"auth":true,"resume":false}}')
    payload = text.encode("ascii")
    assert len(payload) <= INFO_MAX
    return payload


class P4Writer:
    """Builds a P4 console stream the way the firmware's writer does (fakes and fixture replay):
    INFO first, whole TEXT/BLOB chains sliced at 1024, CARD/MIRROR as standalone records, ABORT
    when a chain is cut. Every record passes :class:`Egress` first."""

    def __init__(self) -> None:
        self.stream = bytearray()
        self.egress = Egress()

    def info(self, fw: str | ByteSource | None, model: str | ByteSource | None) -> None:
        self.stream += encode(RecordType.INFO, 0, build_info(fw, model))

    def push(self, rtype: int, flags: int, payload: ByteSource = b"") -> None:
        fault = self.egress.check(rtype, flags, len(payload))
        if fault is not Fault.NONE:
            raise RecordError(f"the P4 writer refuses {type_name(rtype)}: {fault.value}")
        self.stream += encode(rtype, flags, payload)
        self.egress.commit(rtype, flags, len(payload))

    def chain(self, rtype: int, data: ByteSource, cut_after: int | None = None) -> None:
        """One TEXT/BLOB chain (one firmware send); with ``cut_after`` records it ends with ABORT
        instead, as when the writer stalls."""
        if not chain_fits(len(data)):
            raise RecordError(f"a chain is 1..{CHAIN_MAX} bytes, got {len(data)}")
        offset = records = 0
        while offset < len(data):
            if records == cut_after:
                self.push(RecordType.ABORT, 0)
                return
            remaining = len(data) - offset
            n = slice_len(remaining)
            self.push(rtype, slice_flags(remaining), data[offset:offset + n])
            offset += n
            records += 1

    def single(self, rtype: int, data: ByteSource) -> None:
        """CARD (one record) or MIRROR (standalone records of at most 512 B, in order)."""
        cap = CARD_MAX if rtype == RecordType.CARD else MIRROR_MAX
        for offset in range(0, len(data), cap):
            self.push(rtype, 0, data[offset:offset + cap])


# --------------------------------------------------------------------------------------------
# Events
# --------------------------------------------------------------------------------------------

def _json_object(data: bytes) -> dict[str, object] | None:
    """``data`` as a JSON object, or None when it is not one."""
    try:
        value = json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None
    return value if isinstance(value, dict) else None


def _text(data: bytes) -> str:
    return data.decode("utf-8", errors="replace")


@dataclass(frozen=True, slots=True)
class Info:
    """The INFO record: the stream's first record, compact JSON."""

    raw: bytes

    @property
    def text(self) -> str:
        return _text(self.raw)

    @property
    def fields(self) -> dict[str, object] | None:
        return _json_object(self.raw)


@dataclass(frozen=True, slots=True)
class Text:
    """One TEXT message: one whole chain, decoded per chain (UTF-8 may split across records)."""

    data: bytes

    @property
    def text(self) -> str:
        return _text(self.data)

    @property
    def utf8_valid(self) -> bool:
        try:
            self.data.decode("utf-8")
        except UnicodeDecodeError:
            return False
        return True


@dataclass(frozen=True, slots=True)
class Blob:
    """A ``fileread … bin`` window: the BLOB chain's bytes paired with the ``enc:"raw"`` TEXT
    header that follows it. ``header`` is None for an orphan BLOB (no header came before the next
    BLOB or the stream's end); ``data`` is empty for a header whose window had no bytes."""

    data: bytes
    header: bytes | None

    @property
    def header_text(self) -> str:
        return _text(self.header) if self.header is not None else ""

    @property
    def header_fields(self) -> dict[str, object] | None:
        return _json_object(self.header) if self.header is not None else None

    @property
    def length_matches(self) -> bool:
        """The header's ``len`` equals the bytes received."""
        fields = self.header_fields
        return fields is not None and fields.get("len") == len(self.data)


@dataclass(frozen=True, slots=True)
class Card:
    """A CARD: the bare notification JSON object."""

    raw: bytes

    @property
    def text(self) -> str:
        return _text(self.raw)

    @property
    def fields(self) -> dict[str, object] | None:
        return _json_object(self.raw)


@dataclass(frozen=True, slots=True)
class Mirror:
    """One MIRROR record (a mirror buffer, or a <= 512 B piece of a longer one)."""

    data: bytes

    @property
    def text(self) -> str:
        return _text(self.data)


@dataclass(frozen=True, slots=True)
class Abort:
    """ABORT cut an open chain: its output is not confirmed. ``partial`` is what had arrived."""

    chain_type: int
    partial: bytes


@dataclass(frozen=True, slots=True)
class Malformed:
    """The P4 sent a malformed record: the stream is dead, answer with stream RESET 1."""

    fault: Fault
    offset: int              # the record's offset in the stream (from OPEN_OK)
    header: Header | None

    @property
    def reset_code(self) -> int:
        """The stream RESET code to answer with (1)."""
        return STREAM_RESET_MALFORMED

    @property
    def reason(self) -> str:
        """A log-safe description (header fields only, never payload bytes)."""
        if self.header is None:
            return f"{self.fault.value} at offset {self.offset}"
        h = self.header
        return (f"{self.fault.value} at offset {self.offset}: {h.name} (0x{h.type:02x}) "
                f"flags=0x{h.flags:02x} len={h.length}")


type Event = Info | Text | Blob | Card | Mirror | Abort | Malformed
EVENT_TYPES: Final = (Info, Text, Blob, Card, Mirror, Abort, Malformed)


# --------------------------------------------------------------------------------------------
# Stream -> deliveries (the fixtures' "deliveries")
# --------------------------------------------------------------------------------------------

class DeliveryKind(Enum):
    INFO = "info"
    TEXT = "text"
    BLOB = "blob"
    CARD = "card"
    MIRROR = "mirror"
    ABORT = "abort"


@dataclass(frozen=True, slots=True)
class Delivery:
    """What the phone's receiver hands on, as the X4 fixture lists it: one per INFO, CARD and
    MIRROR record, one per whole TEXT/BLOB chain, and one per ABORT (the cut chain's bytes so
    far; ``chain_type`` says which type was cut)."""

    kind: DeliveryKind
    data: bytes
    chain_type: int = 0


@dataclass(frozen=True, slots=True)
class SeenRecord:
    """Trace entry: one record the receiver examined (up to and including a malformed one)."""

    offset: int
    scan: Scan


_SINGLE_KINDS: Final = {RecordType.INFO: DeliveryKind.INFO, RecordType.CARD: DeliveryKind.CARD,
                        RecordType.MIRROR: DeliveryKind.MIRROR}


class RecordAssembler:
    """Turns the console stream's bytes (any split) into :class:`Delivery` items and at most one
    :class:`Malformed`, using :class:`PhoneReceiver`. A partial record waits in a buffer; after a
    malformed record everything is dropped until :meth:`reset` (a new stream)."""

    def __init__(self, keep_trace: bool = False) -> None:
        self._keep_trace = keep_trace
        self.reset()

    def reset(self) -> None:
        """Start a new console stream (a fresh OPEN_OK)."""
        self._receiver = PhoneReceiver()
        self._buffer = bytearray()
        self._offset = 0                 # stream offset of _buffer[0]
        self._chain = bytearray()        # the open chain's payload so far
        self._chain_type = 0
        self.records = 0                 # whole records consumed (delivered, skipped or ignored)
        self.skipped = 0                 # unknown and reserved types
        self.ignored = 0                 # stray ABORTs
        self.dropped = 0                 # bytes discarded after a malformed record
        self.malformed: Malformed | None = None
        self.trace: list[SeenRecord] = []

    @property
    def receiver(self) -> PhoneReceiver:
        return self._receiver

    @property
    def buffered(self) -> int:
        """Bytes of a partial record waiting for the rest."""
        return len(self._buffer)

    @property
    def consumed(self) -> int:
        """Stream bytes consumed as whole records."""
        return self._offset

    def feed(self, data: ByteSource) -> list[Delivery | Malformed]:
        if self.malformed is not None:
            self.dropped += len(data)
            return []
        self._buffer += data
        out: list[Delivery | Malformed] = []
        while True:
            r = self._receiver.next(self._buffer)
            if r.verdict is Verdict.NEED_MORE:
                return out
            if self._keep_trace:
                self.trace.append(SeenRecord(self._offset, r))
            if r.verdict is Verdict.MALFORMED:
                self.malformed = Malformed(r.fault, self._offset, r.header)
                self.dropped += len(self._buffer)
                self._buffer.clear()
                self._chain.clear()
                out.append(self.malformed)
                return out
            delivery = self._consume(r)
            if delivery is not None:
                out.append(delivery)

    def _consume(self, r: Scan) -> Delivery | None:
        """Apply a whole record (Deliver, Skip or Ignore) and drop it from the buffer."""
        h = r.header
        assert h is not None
        payload = bytes(self._buffer[HEADER_SIZE:r.size])
        del self._buffer[:r.size]
        self._offset += r.size
        self.records += 1
        if r.verdict is Verdict.SKIP:
            self.skipped += 1
            return None
        if r.verdict is Verdict.IGNORE:
            self.ignored += 1
            return None
        if h.type in _SINGLE_KINDS:
            return Delivery(_SINGLE_KINDS[h.type], payload)
        if h.type == RecordType.ABORT:
            cut = Delivery(DeliveryKind.ABORT, bytes(self._chain), self._chain_type)
            self._chain.clear()
            self._chain_type = 0
            return cut
        # TEXT or BLOB: one delivery per whole chain.
        self._chain += payload
        self._chain_type = h.type
        if not r.chain_end:
            return None
        kind = DeliveryKind.TEXT if h.type == RecordType.TEXT else DeliveryKind.BLOB
        whole = Delivery(kind, bytes(self._chain))
        self._chain.clear()
        self._chain_type = 0
        return whole


# --------------------------------------------------------------------------------------------
# The console endpoint
# --------------------------------------------------------------------------------------------

def is_raw_header(data: bytes) -> bool:
    """A TEXT message that is a ``fileread … bin`` header: a JSON object with ``"enc":"raw"``."""
    if not data.startswith(b"{") or b"raw" not in data:
        return False
    fields = _json_object(data)
    return fields is not None and fields.get("enc") == "raw"


@dataclass(slots=True)
class ConsoleStats:
    """Counters for one console stream (reset with the stream)."""

    bytes_in: int = 0
    records: int = 0
    skipped: int = 0
    ignored: int = 0
    messages: int = 0          # events handed on (Malformed excluded)
    aborts: int = 0
    orphan_blobs: int = 0
    commands_built: int = 0
    binaries_built: int = 0


class ConsoleEndpoint:
    """The phone's end of one console stream: P4 bytes in, :data:`Event` items out; COMMAND and
    BINARY records built for the way back.

    * :meth:`feed` takes the stream's bytes as DATA frames deliver them (any split) and returns
      the events completed by them, in order.
    * A BLOB chain is held until its ``enc:"raw"`` TEXT header arrives (TEXT, CARD and MIRROR in
      between pass through), then handed on as one :class:`Blob`. A raw header with no BLOB
      before it (an empty window) gives ``Blob(b"", header)``; a BLOB superseded by another
      BLOB, or still held at :meth:`flush`, gives ``Blob(data, None)``.
    * A malformed record gives one :class:`Malformed` (answer it with stream RESET 1); later
      bytes are dropped until :meth:`reset`.
    """

    def __init__(self) -> None:
        self._assembler = RecordAssembler()
        self.reset()

    def reset(self) -> None:
        """Start a new console stream (a fresh OPEN_OK): nothing seen, signed out, no INFO."""
        self._assembler.reset()
        self._pending_blob: bytes | None = None
        self.info: Info | None = None
        self.stats = ConsoleStats()

    @property
    def failed(self) -> bool:
        return self._assembler.malformed is not None

    @property
    def malformed(self) -> Malformed | None:
        return self._assembler.malformed

    @property
    def pending_blob(self) -> bytes | None:
        """A BLOB still waiting for its header."""
        return self._pending_blob

    @property
    def buffered(self) -> int:
        """Bytes of a partial record waiting for the rest."""
        return self._assembler.buffered

    @property
    def chain_open(self) -> bool:
        return self._assembler.receiver.chain_open

    def feed(self, data: ByteSource) -> list[Event]:
        self.stats.bytes_in += len(data)
        events: list[Event] = []
        for item in self._assembler.feed(data):
            events.extend(self._events(item))
        a = self._assembler
        self.stats.records, self.stats.skipped, self.stats.ignored = a.records, a.skipped, a.ignored
        self.stats.messages += sum(1 for e in events if not isinstance(e, Malformed))
        return events

    def flush(self) -> list[Event]:
        """The stream ended: hand on a BLOB that never got its header."""
        if self._pending_blob is None:
            return []
        orphan = Blob(self._pending_blob, None)
        self._pending_blob = None
        self.stats.orphan_blobs += 1
        return [orphan]

    def _events(self, item: Delivery | Malformed) -> list[Event]:
        if isinstance(item, Malformed):
            return [item]
        match item.kind:
            case DeliveryKind.INFO:
                self.info = Info(item.data)
                return [self.info]
            case DeliveryKind.TEXT:
                if is_raw_header(item.data):
                    blob = Blob(self._pending_blob or b"", item.data)
                    self._pending_blob = None
                    return [blob]
                return [Text(item.data)]
            case DeliveryKind.BLOB:
                orphans = self.flush()
                self._pending_blob = item.data
                return orphans
            case DeliveryKind.CARD:
                return [Card(item.data)]
            case DeliveryKind.MIRROR:
                return [Mirror(item.data)]
            case DeliveryKind.ABORT:
                self.stats.aborts += 1
                return [Abort(item.chain_type, item.data)]
        raise AssertionError(f"unhandled delivery {item.kind}")

    def command(self, line: str | ByteSource) -> bytes:
        """A COMMAND record for ``line`` (see :func:`build_command`)."""
        record = build_command(line)
        self.stats.commands_built += 1
        return record

    def binary(self, frame: ByteSource) -> bytes:
        """A BINARY record for one OtaBleWire frame (see :func:`build_binary`)."""
        record = build_binary(frame)
        self.stats.binaries_built += 1
        return record
