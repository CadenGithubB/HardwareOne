"""Tests for hw1peer.records: the console records, phone side.

Replays every X4 record vector (``phone_console_v2_records.json``) byte for byte:

* ``p4_to_phone``: the phone's verdict per record, the deliveries, the stream verdict and fault
  offset, the receiver action and the skipped/ignored counts, over the whole stream and over
  arbitrary DATA splits (one byte at a time included);
* ``p4_production`` streams rebuilt with the P4-side writer from the generator's inputs;
* ``phone_to_p4``: the P4's ingress verdict per record, and the phone's builders producing exactly
  the delivered records (and refusing the payloads the P4 calls malformed).

Then the contract rules one by one, and the endpoint's events (INFO, TEXT, BLOB pairing, CARD,
MIRROR, ABORT, Malformed). Plain functions; the package runner calls every ``test_*``.
"""

import hashlib
import random
from typing import Any

from hw1peer import records as rec
from hw1peer.tests import FIXTURES, load_fixture
from hw1peer.records import (
    Abort, Blob, Card, ConsoleEndpoint, Delivery, DeliveryKind, Egress, Fault, Header, Info, Malformed,
    Mirror, P4Writer, PhoneReceiver, RecordAssembler, RecordError, RecordType, Side, Text, Verdict,
)

X4_NAME = "phone_console_v2_records.json"
X4_SHA256 = "781868582f5e876795d6d20fac35d1682cf84c17f3e35048aa2a7b3694a65f2e"  # firmware-009 §3


def check(condition: object, message: str = "") -> None:
    """An assert that survives ``python -O``."""
    if not condition:
        raise AssertionError(message)


def check_eq(got: object, want: object, what: str) -> None:
    if got != want:
        raise AssertionError(f"{what}: got {got!r}, want {want!r}")


def raises(error: type[BaseException], fn: Any, *args: Any) -> None:
    try:
        fn(*args)
    except error:
        return
    raise AssertionError(f"{getattr(fn, '__name__', fn)}{args!r:.80} did not raise {error.__name__}")


def x4() -> dict[str, Any]:
    return load_fixture(X4_NAME)


def vectors(direction: str) -> list[dict[str, Any]]:
    return [v for v in x4()["vectors"] if v["direction"] == direction]


def stream_of(vector: dict[str, Any]) -> bytes:
    return bytes.fromhex(vector["stream_hex"])


def utf8_or_none(data: bytes) -> str | None:
    """The fixtures' ``utf8`` field: the bytes decoded as UTF-8, or null."""
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


# --------------------------------------------------------------------------------------------
# Fixture pin and constants
# --------------------------------------------------------------------------------------------

def test_fixture_sha256_pinned() -> None:
    check_eq(hashlib.sha256((FIXTURES / X4_NAME).read_bytes()).hexdigest(), X4_SHA256, "X4 SHA-256")


def test_constants_match_fixture() -> None:
    d = x4()
    check_eq(d["types"], {t.name: t.value for t in RecordType}, "type codes")
    limits = d["limits"]
    want = {"payload": rec.PAYLOAD_MAX, "chain": rec.CHAIN_MAX, "info": rec.INFO_MAX, "text": rec.TEXT_MAX,
            "card": rec.CARD_MAX, "mirror": rec.MIRROR_MAX, "command": rec.COMMAND_MAX,
            "binary_min": rec.BINARY_MIN, "binary_max": rec.BINARY_MAX, "unknown": rec.UNKNOWN_MAX,
            "info_field": rec.INFO_FIELD}
    check_eq(limits, want, "limits")
    check_eq(bytes.fromhex(d["binary_prefix_hex"]), rec.OTA_MAGIC, "BINARY prefix")
    check_eq(rec.OTA_HEAD_SIZE + rec.OTA_CHUNK_MAX, rec.BINARY_MAX, "OTA head + chunk")
    check_eq(rec.SCAN_PEEK, 12, "scan peek")
    check_eq(len(d["vectors"]), 73, "X4 record vectors (firmware-009 §3)")


# --------------------------------------------------------------------------------------------
# Header codec and type table
# --------------------------------------------------------------------------------------------

def test_header_examples_from_firmware_009() -> None:
    check_eq(rec.encode(RecordType.TEXT, 0, b"OK"), bytes.fromhex("020002004f4b"), "TEXT OK")
    check_eq(rec.encode(RecordType.COMMAND, 0, b"help"), bytes.fromhex("8100040068656c70"), "COMMAND help")
    check_eq(rec.encode(RecordType.ABORT, 0), bytes.fromhex("06000000"), "ABORT")
    check_eq(rec.build_command("help"), bytes.fromhex("8100040068656c70"), "build_command help")


def test_header_codec_little_endian() -> None:
    check_eq(rec.encode_header(0x02, 0x01, 0x01FF), bytes((0x02, 0x01, 0xFF, 0x01)), "len LE")
    h = rec.parse_header(bytes((0x03, 0x01, 0x00, 0x04, 0xAA)))
    check_eq(h, Header(0x03, 0x01, 1024), "parse")
    check(h.more and h.name == "BLOB" and h.size == 1028, "header properties")
    check_eq(Header(0x81, 0, 511).encode(), bytes((0x81, 0, 0xFF, 0x01)), "Header.encode")
    for length in (0, 1, 255, 256, 1024, 0xFFFF):
        check_eq(rec.parse_header(rec.encode_header(0x40, 0, length)).length, length, f"roundtrip {length}")
    raises(RecordError, rec.parse_header, b"\x02\x00\x01")
    raises(RecordError, rec.encode_header, 0x02, 0, 0x10000)
    raises(RecordError, rec.encode_header, 0x100, 0, 1)
    raises(RecordError, rec.encode, 0x02, 0, bytes(0x10000))


def test_type_table() -> None:
    to_phone, to_p4, unknown = rec.Direction.TO_PHONE, rec.Direction.TO_P4, rec.Direction.UNKNOWN
    rows = {  # type: (direction, reserved, more, min, max), as BLE_PhonePipeConsoleCore.h typeSpec()
        0x01: (to_phone, False, False, 1, 256), 0x02: (to_phone, False, True, 1, 1024),
        0x03: (to_phone, False, True, 1, 1024), 0x04: (to_phone, False, False, 1, 512),
        0x05: (to_phone, False, False, 1, 512), 0x06: (to_phone, False, False, 0, 0),
        0x07: (to_phone, True, False, 0, 1024), 0x81: (to_p4, False, False, 1, 511),
        0x82: (to_p4, False, False, 13, 489), 0x83: (to_p4, True, False, 0, 1024),
    }
    for code in range(256):
        s = rec.type_spec(code)
        check_eq((s.direction, s.reserved, s.more, s.min_len, s.max_len),
                 rows.get(code, (unknown, False, False, 0, 1024)), f"typeSpec 0x{code:02x}")
        name = rec.type_name(code)
        check_eq(name, RecordType(code).name if code in rows else "UNKNOWN", f"typeName 0x{code:02x}")
        check_eq(rec.is_skipped(Side.PHONE, code), code not in rows or code == 0x07, f"skip at phone 0x{code:02x}")
        check_eq(rec.is_skipped(Side.P4, code), code not in rows or code == 0x83, f"skip at P4 0x{code:02x}")


def test_header_fault_every_flag_bit() -> None:
    for bit in range(1, 8):
        h = Header(RecordType.TEXT, 1 << bit, 2)
        check_eq(rec.header_fault(Side.PHONE, h), Fault.FLAGS, f"flag bit {bit} at the phone")
        check_eq(rec.header_fault(Side.P4, Header(RecordType.COMMAND, 1 << bit, 2)), Fault.FLAGS, f"bit {bit} at P4")
        check_eq(rec.header_fault(Side.PHONE, Header(RecordType.TEXT, (1 << bit) | rec.MORE, 2)), Fault.FLAGS,
                 f"flag bit {bit} with MORE")
    check_eq(rec.header_fault(Side.PHONE, Header(RecordType.TEXT, rec.MORE, 2)), Fault.NONE, "MORE alone")


def test_scan_bounds_every_type_at_the_phone() -> None:
    for code in range(256):
        spec = rec.type_spec(code)
        wrong_way = spec.direction is rec.Direction.TO_P4
        for length in sorted({0, 1, spec.min_len, spec.max_len, spec.max_len + 1}):
            r = rec.scan(Side.PHONE, rec.encode(code, 0, bytes(length)))
            if wrong_way:
                want = Fault.DIRECTION
            elif not spec.min_len <= length <= spec.max_len:
                want = Fault.LENGTH
            else:
                want = Fault.NONE
            check_eq(r.fault, want, f"0x{code:02x} len {length}")
            if want is Fault.NONE:
                check_eq(r.verdict, Verdict.SKIP if rec.is_skipped(Side.PHONE, code) else Verdict.DELIVER,
                         f"0x{code:02x} len {length} verdict")


# --------------------------------------------------------------------------------------------
# X4 p4_to_phone: the phone's verdicts
# --------------------------------------------------------------------------------------------

def phone_side(stream: bytes) -> dict[str, Any]:
    """The generator's phoneSide() over the whole stream, in the fixture's own shape."""
    rx = PhoneReceiver()
    seen: list[dict[str, Any]] = []
    deliveries: list[dict[str, Any]] = []
    chain = b""
    out: dict[str, Any] = {"verdict": "ok", "fault": "none", "fault_offset": -1, "skipped": 0, "ignored": 0}
    offset = 0
    while offset < len(stream):
        r = rx.next(stream[offset:])
        check(r.header is not None and r.verdict is not Verdict.NEED_MORE, f"whole stream at {offset}")
        assert r.header is not None
        payload = stream[offset + rec.HEADER_SIZE:offset + r.size]
        seen.append({"offset": offset, "type": r.header.name, "code": r.header.type, "flags": r.header.flags,
                     "len": r.header.length, "payload_hex": payload.hex(), "verdict": r.verdict.value,
                     "fault": r.fault.value})
        if r.verdict is Verdict.MALFORMED:
            out.update(verdict="malformed", fault=r.fault.value, fault_offset=offset)
            break
        kind = None
        if r.verdict is Verdict.SKIP:
            out["skipped"] += 1
        elif r.verdict is Verdict.IGNORE:
            out["ignored"] += 1
        elif r.header.type in (RecordType.TEXT, RecordType.BLOB):
            chain += payload
            if r.chain_end:
                kind, payload, chain = ("text" if r.header.type == RecordType.TEXT else "blob"), chain, b""
        elif r.header.type == RecordType.ABORT:
            kind, payload, chain = "abort", chain, b""
        else:
            kind = r.header.name.lower()
        if kind:
            deliveries.append({"kind": kind, "hex": payload.hex(), "utf8": utf8_or_none(payload)})
        offset += r.size
    out["records"] = seen
    out["deliveries"] = deliveries
    out["receiver_action"] = "stream_reset_1" if out["verdict"] == "malformed" else "none"
    return out


def test_x4_p4_to_phone_whole_stream() -> None:
    count = 0
    for v in vectors("p4_to_phone"):
        got = phone_side(stream_of(v))
        for key in ("records", "deliveries", "verdict", "fault", "fault_offset", "receiver_action", "skipped",
                    "ignored"):
            check_eq(got[key], v[key], f"{v['id']}: {key}")
        count += 1
    check_eq(count, 51, "p4_to_phone vectors")


def assembled(stream: bytes, chunks: list[int]) -> tuple[RecordAssembler, list[Delivery | Malformed]]:
    """Feed ``stream`` to a fresh tracing assembler in pieces of the given sizes (cycled)."""
    a = RecordAssembler(keep_trace=True)
    items: list[Delivery | Malformed] = []
    offset = i = 0
    while offset < len(stream):
        n = max(1, chunks[i % len(chunks)])
        items += a.feed(stream[offset:offset + n])
        offset += n
        i += 1
    return a, items


def split_patterns(stream: bytes, rng: random.Random) -> list[list[int]]:
    """Whole, one byte at a time, BLE-ish payload sizes, and random splits."""
    patterns = [[len(stream) or 1], [1], [2], [3], [5], [20], [178], [235], [508]]
    for _ in range(4):
        patterns.append([rng.randint(1, 64) for _ in range(17)])
        patterns.append([rng.randint(1, 1100) for _ in range(7)])
    return patterns


def expected_deliveries(v: dict[str, Any]) -> list[tuple[str, str]]:
    return [(d["kind"], d["hex"]) for d in v["deliveries"]]


def test_x4_p4_to_phone_any_split() -> None:
    rng = random.Random(0x4E4)
    for v in vectors("p4_to_phone"):
        stream = stream_of(v)
        for chunks in split_patterns(stream, rng):
            a, items = assembled(stream, chunks)
            got = [(d.kind.value, d.data.hex()) for d in items if isinstance(d, Delivery)]
            where = f"{v['id']} split {chunks[:4]}"
            check_eq(got, expected_deliveries(v), f"{where}: deliveries")
            check_eq((a.skipped, a.ignored), (v["skipped"], v["ignored"]), f"{where}: skipped/ignored")
            bad = [m for m in items if isinstance(m, Malformed)]
            if v["verdict"] == "ok":
                check(not bad and a.buffered == 0 and a.consumed == len(stream), f"{where}: clean end")
            else:
                check_eq(len(bad), 1, f"{where}: one Malformed")
                check_eq((bad[0].fault.value, bad[0].offset), (v["fault"], v["fault_offset"]), f"{where}: fault")
                check(items[-1] is bad[0] and bad[0].reset_code == 1, f"{where}: Malformed is last, RESET 1")
            trace = [(s.offset, s.scan.header.name if s.scan.header else None, s.scan.header.flags
                      if s.scan.header else None, s.scan.header.length if s.scan.header else None,
                      s.scan.verdict.value, s.scan.fault.value) for s in a.trace]
            want = [(r["offset"], r["type"], r["flags"], r["len"], r["verdict"], r["fault"]) for r in v["records"]]
            check_eq(trace, want, f"{where}: records")


def test_x4_verdict_never_goes_back() -> None:
    """For every record of every p4_to_phone vector, growing prefixes say NeedMore until the final
    verdict; a malformed record shows exactly when its header is complete, never earlier."""
    for v in vectors("p4_to_phone"):
        stream = stream_of(v)
        rx = PhoneReceiver()
        for r in v["records"]:
            start, final = r["offset"], Verdict(r["verdict"])
            size = rec.HEADER_SIZE + r["len"]
            for have in range(0, min(size, len(stream) - start) + 1):
                got = rx.copy().next(stream[start:start + have])
                if have < rec.HEADER_SIZE:
                    want = Verdict.NEED_MORE
                elif final is Verdict.MALFORMED:
                    want = Verdict.MALFORMED
                else:
                    want = final if have == size else Verdict.NEED_MORE
                check_eq(got.verdict, want, f"{v['id']} record at {start}, {have} bytes")
            if final is not Verdict.MALFORMED:
                check_eq(rx.next(stream[start:start + size]).verdict, final, f"{v['id']} advance at {start}")


def test_x4_malformed_latches() -> None:
    for v in vectors("p4_to_phone"):
        if v["verdict"] != "malformed":
            continue
        stream = stream_of(v)
        rx = PhoneReceiver()
        offset = 0
        while offset < len(stream):
            r = rx.next(stream[offset:])
            if r.verdict is Verdict.MALFORMED:
                break
            offset += r.size
        check(rx.failed and rx.fault.value == v["fault"] and offset == v["fault_offset"], f"{v['id']}: latched")
        later = rx.next(rec.encode(RecordType.TEXT, 0, b"OK"))
        check_eq((later.verdict, later.fault.value), (Verdict.MALFORMED, v["fault"]), f"{v['id']}: stays malformed")
        check_eq(rx.next(b"").verdict, Verdict.MALFORMED, f"{v['id']}: even with no bytes")


# --------------------------------------------------------------------------------------------
# X4 p4_production streams rebuilt by the P4-side writer (generator inputs)
# --------------------------------------------------------------------------------------------

def lines(n: int, seed: int) -> bytes:
    """phone_console_vectors.cpp lines(): 63 letters and LF per 64 bytes."""
    return bytes(0x0A if i % 64 == 63 else ord("a") + (seed + i) % 26 for i in range(n))


def pattern(n: int, seed: int) -> bytes:
    return bytes((seed + i * 7) & 0xFF for i in range(n))


def help_lines(count: int) -> list[bytes]:
    return [f"  cmd{i:02d}{'':{4 + i % 5}} synthetic help line {i:02d} for the console fixture".encode()
            for i in range(count)]


def raw_header(size: int, offset: int, length: int, eof: bool) -> bytes:
    return (f'{{"success":true,"path":"/fixture.bin","size":{size},"offset":{offset},"len":{length},'
            f'"eof":{"true" if eof else "false"},"enc":"raw"}}').encode()


CARD = b'{"kind":"system","level":"info","title":"Fixture","msg":"synthetic card","ms":4000}'
FW, MODEL = "0.0.0-fixture", "HardwareOne"


def output(w: P4Writer, line_list: list[bytes], result: bytes) -> None:
    """The generator's Writer::output(): whole-line TEXT batches of at most 512 B, then the result."""
    batch = b""
    for line in line_list:
        if len(batch) + len(line) + 1 > 512:
            w.chain(RecordType.TEXT, batch)
            batch = b""
        batch += line + b"\n"
    if batch:
        w.chain(RecordType.TEXT, batch)
    w.chain(RecordType.TEXT, result)


def production_streams() -> dict[str, bytes]:
    T, B = RecordType.TEXT, RecordType.BLOB
    built: dict[str, bytes] = {}

    def make(name: str, *steps: Any, fw: Any = FW, model: Any = MODEL) -> None:
        w = P4Writer()
        w.info(fw, model)
        for step in steps:
            step(w)
        built[name] = bytes(w.stream)

    make("info")
    make("info_escaped", fw=b'0.0.0-"q"\\\t\x01\x7f\xc3\xa9', model=None)
    make("info_long_fields", fw="f" * 80, model="m" * 63 + '"tail')
    make("text_one_record", lambda w: w.chain(T, b"OK: synthetic reply"))
    make("text_4096", lambda w: w.chain(T, lines(4096, 1)))
    make("text_4608_cap", lambda w: w.chain(T, lines(rec.CHAIN_MAX, 2)))
    make("help_output", lambda w: output(w, help_lines(24), b"OK"))
    make("help_all_output", lambda w: output(w, help_lines(80), b"OK"))
    make("blob_then_raw_header", lambda w: w.chain(B, pattern(1300, 0x42)),
         lambda w: w.chain(T, raw_header(5000, 0, 1300, False)))
    make("blob_output_then_header", lambda w: w.chain(B, pattern(700, 0x43)),
         lambda w: w.chain(T, b"synthetic output line\n"), lambda w: w.chain(T, raw_header(5000, 4300, 700, True)))
    make("blob_4608", lambda w: w.chain(B, pattern(rec.CHAIN_MAX, 0x44)),
         lambda w: w.chain(T, raw_header(rec.CHAIN_MAX, 0, rec.CHAIN_MAX, True)))
    make("card", lambda w: w.single(RecordType.CARD, CARD))
    make("mirror", lambda w: w.single(RecordType.MIRROR, lines(100, 4)))
    make("mirror_split", lambda w: w.single(RecordType.MIRROR, lines(1100, 5)))
    make("abort_mid_chain", lambda w: w.chain(T, lines(1500, 6), cut_after=1))
    make("abort_then_next", lambda w: w.chain(T, lines(3000, 7), cut_after=2), lambda w: w.chain(T, b"OK: next reply"))
    make("card_mirror_between_chains", lambda w: w.chain(T, lines(1500, 8)), lambda w: w.single(RecordType.CARD, CARD),
         lambda w: w.single(RecordType.MIRROR, lines(40, 9)), lambda w: w.chain(T, b"OK"))
    return built


def test_x4_p4_production_rebuilt_byte_for_byte() -> None:
    built = production_streams()
    production = [v for v in vectors("p4_to_phone") if v["source"] == "p4_production"]
    check_eq(sorted(built), sorted(v["id"] for v in production), "every p4_production vector rebuilt")
    for v in production:
        check_eq(built[v["id"]].hex(), v["stream_hex"], f"{v['id']}: stream")


def test_build_info() -> None:
    info = rec.build_info(FW, MODEL)
    check_eq(info, b'{"v":1,"fw":"0.0.0-fixture","model":"HardwareOne","line":511,"bin":489,"auth":true,'
                   b'"resume":false}', "INFO bytes")
    check_eq(rec.build_info(FW, ""), info, "empty model is HardwareOne")
    check_eq(rec.build_info(FW, b"\x00x"), info, "a C string ends at NUL")
    longest = rec.build_info("\x01" * 100, "\x01" * 100)
    check(len(longest) <= rec.INFO_MAX and longest.count(b"\\u0001") == 2 * 10, "cut at 64 escaped bytes")


# --------------------------------------------------------------------------------------------
# X4 phone_to_p4: the P4's verdicts and the phone's builders
# --------------------------------------------------------------------------------------------

def test_x4_phone_to_p4_p4_verdicts() -> None:
    """The generator's p4Side() with this module's scan_ingress(): record by record."""
    count = 0
    for v in vectors("phone_to_p4"):
        stream = stream_of(v)
        seen, deliveries, skipped = [], [], 0
        verdict, fault, fault_offset = "ok", "none", -1
        offset = 0
        while offset < len(stream):
            r = rec.scan_ingress(stream[offset:])
            assert r.header is not None
            payload = stream[offset + rec.HEADER_SIZE:offset + r.size]
            seen.append({"offset": offset, "type": r.header.name, "code": r.header.type, "flags": r.header.flags,
                         "len": r.header.length, "payload_hex": payload.hex(), "verdict": r.verdict.value,
                         "fault": r.fault.value})
            if r.verdict is Verdict.MALFORMED:
                verdict, fault, fault_offset = "malformed", r.fault.value, offset
                break
            check(r.verdict in (Verdict.DELIVER, Verdict.SKIP), f"{v['id']}: whole records")
            if r.verdict is Verdict.SKIP:
                skipped += 1
            else:
                kind = "command" if r.header.type == RecordType.COMMAND else "binary"
                deliveries.append({"kind": kind, "hex": payload.hex(), "utf8": utf8_or_none(payload)})
            offset += r.size
        where = v["id"]
        check_eq(seen, v["records"], f"{where}: records")
        check_eq(deliveries, v["deliveries"], f"{where}: deliveries")
        check_eq((verdict, fault, fault_offset, skipped), (v["verdict"], v["fault"], v["fault_offset"], v["skipped"]),
                 f"{where}: verdict")
        check_eq(v["receiver_action"], "close_1" if verdict == "malformed" else "none", f"{where}: action")
        count += 1
    check_eq(count, 22, "phone_to_p4 vectors")


def test_x4_magic_shows_on_its_byte() -> None:
    v = next(v for v in vectors("phone_to_p4") if v["id"] == "binary_bad_magic")
    stream = stream_of(v)
    for have in range(len(stream) + 1):
        want = Verdict.NEED_MORE if have < 6 else Verdict.MALFORMED  # payload byte 1 is the bad one
        check_eq(rec.scan_ingress(stream[:have]).verdict, want, f"bad magic, {have} bytes")


def build_from_delivery(kind: str, payload: bytes) -> bytes:
    return rec.build_command(payload) if kind == "command" else rec.build_binary(payload)


def test_x4_phone_builds_its_records_byte_for_byte() -> None:
    built_ok = refused = 0
    for v in vectors("phone_to_p4"):
        stream = stream_of(v)
        where = v["id"]
        if v["verdict"] == "ok":
            # Rebuild the stream: the phone's builders for COMMAND/BINARY, raw encode() for the
            # skipped records the phone never sends (RESUME, unknown).
            rebuilt = b""
            for r in v["records"]:
                payload = bytes.fromhex(r["payload_hex"])
                if r["verdict"] == "skip":
                    rebuilt += rec.encode(r["code"], r["flags"], payload)
                else:
                    rebuilt += build_from_delivery(r["type"].lower(), payload)
                    built_ok += 1
            check_eq(rebuilt.hex(), v["stream_hex"], f"{where}: rebuilt")
            continue
        r = v["records"][-1]
        payload = bytes.fromhex(r["payload_hex"])
        if r["code"] not in (RecordType.COMMAND, RecordType.BINARY):
            continue  # a type the phone has no builder for (direction, unknown)
        if v["fault"] in ("length", "magic"):
            raises(RecordError, build_from_delivery, r["type"].lower(), payload)
            refused += 1
        else:  # more / flags: the builder never sets them
            record = build_from_delivery(r["type"].lower(), payload)
            check(record[1] == 0 and record != stream, f"{where}: builder writes flags 0")
            check_eq(rec.scan_ingress(record).verdict, Verdict.DELIVER, f"{where}: the clean record is delivered")
    check_eq((built_ok, refused), (10, 5), "records built / payloads refused")


# --------------------------------------------------------------------------------------------
# Phone builders
# --------------------------------------------------------------------------------------------

def test_build_command_bounds() -> None:
    check_eq(rec.build_command("x"), bytes.fromhex("8100010078"), "1 byte")
    check_eq(len(rec.build_command("y" * 511)), 515, "511 bytes")
    raises(RecordError, rec.build_command, "")
    raises(RecordError, rec.build_command, "y" * 512)
    raises(RecordError, rec.build_command, "status\n")
    raises(RecordError, rec.build_command, "a\nb")
    check_eq(rec.build_command("é" * 255)[2:4], (510).to_bytes(2, "little"), "UTF-8 length in bytes")
    raises(RecordError, rec.build_command, "é" * 256)
    check_eq(rec.build_command(b"ls\t/\r\x00"), bytes.fromhex("810006006c73092f0d00"), "raw bytes unchanged")
    for line in ("help", "y" * 511, "ls /"):
        check_eq(rec.scan_ingress(rec.build_command(line)).verdict, Verdict.DELIVER, f"P4 accepts {line[:8]!r}")


def test_build_binary_and_ota_frame() -> None:
    frame = rec.ota_frame(0, b"\x90")
    check_eq(frame, bytes.fromhex("004857314f5441010000000090"), "smallest frame")
    check_eq(rec.build_binary(frame), bytes.fromhex("82000d00004857314f5441010000000090"), "binary_13")
    check_eq(rec.ota_frame(4096, b"\x01")[8:12], bytes.fromhex("00001000"), "offset u32 BE")
    check_eq(len(rec.build_binary(rec.ota_frame(0xFFFFFFFF, bytes(477)))), 493, "largest frame")
    raises(RecordError, rec.ota_frame, 0, b"")
    raises(RecordError, rec.ota_frame, 0, bytes(478))
    raises(RecordError, rec.ota_frame, -1, b"\x00")
    raises(RecordError, rec.ota_frame, 1 << 32, b"\x00")
    raises(RecordError, rec.build_binary, rec.OTA_MAGIC + bytes(4))
    raises(RecordError, rec.build_binary, rec.OTA_MAGIC + bytes(482))
    raises(RecordError, rec.build_binary, b"\x01HW1OTA\x01" + bytes(5))
    for size in (13, 100, 489):
        record = rec.build_binary(rec.OTA_MAGIC + bytes(size - 8))
        check_eq(rec.scan_ingress(record).verdict, Verdict.DELIVER, f"P4 accepts BINARY {size}")


# --------------------------------------------------------------------------------------------
# Contract rules, one by one, through the endpoint
# --------------------------------------------------------------------------------------------

INFO_RECORD = rec.encode(RecordType.INFO, 0, rec.build_info(FW, MODEL))


def r(rtype: int, flags: int = 0, payload: bytes = b"") -> bytes:
    return rec.encode(rtype, flags, payload)


def events_of(*parts: bytes, endpoint: ConsoleEndpoint | None = None) -> list[Any]:
    e = endpoint or ConsoleEndpoint()
    out: list[Any] = []
    for part in parts:
        out += e.feed(part)
    return out


def fault_of(*parts: bytes) -> tuple[str, int] | None:
    bad = [e for e in events_of(*parts) if isinstance(e, Malformed)]
    return (bad[0].fault.value, bad[0].offset) if bad else None


def test_rule_info_first_and_once() -> None:
    check_eq(fault_of(r(RecordType.TEXT, 0, b"OK")), ("info-order", 0), "TEXT first")
    check_eq(fault_of(r(RecordType.CARD, 0, CARD)), ("info-order", 0), "CARD first")
    check_eq(fault_of(r(RecordType.ABORT)), ("info-order", 0), "ABORT first")
    check_eq(fault_of(INFO_RECORD, INFO_RECORD), ("info-order", len(INFO_RECORD)), "second INFO")
    check_eq(fault_of(r(0x40, 0, b"x"), INFO_RECORD), None, "an unknown type before INFO is skipped")
    check_eq(fault_of(r(RecordType.TICKET, 0, b"x"), INFO_RECORD), None, "TICKET before INFO is skipped")
    evs = events_of(INFO_RECORD)
    check(len(evs) == 1 and isinstance(evs[0], Info), "INFO event")
    fields = evs[0].fields
    check(fields is not None and list(fields) == ["v", "fw", "model", "line", "bin", "auth", "resume"], "INFO keys")
    check(fields is not None and fields["line"] == 511 and fields["bin"] == 489, "INFO limits")


def test_rule_more_only_on_text_and_blob() -> None:
    for rtype, payload in ((RecordType.INFO, b"{}"), (RecordType.CARD, CARD), (RecordType.MIRROR, b"m"),
                           (0x40, b"abc"), (RecordType.TICKET, b"t")):
        start = b"" if rtype == RecordType.INFO else INFO_RECORD
        check_eq(fault_of(start + r(rtype, rec.MORE, payload)), ("more", len(start)), f"MORE on {rec.type_name(rtype)}")
    open_chain = INFO_RECORD + r(RecordType.TEXT, rec.MORE, b"a")
    check_eq(fault_of(open_chain + r(RecordType.ABORT, rec.MORE)), ("more", len(open_chain)), "MORE on ABORT")
    check_eq(fault_of(INFO_RECORD + r(RecordType.TEXT, rec.MORE, b"a") + r(RecordType.TEXT, 0, b"b")), None,
             "MORE on TEXT")
    check_eq(fault_of(INFO_RECORD + r(RecordType.BLOB, rec.MORE, b"a") + r(RecordType.BLOB, 0, b"b")), None,
             "MORE on BLOB")


def test_rule_flag_bits_1_to_7() -> None:
    for bit in range(1, 8):
        check_eq(fault_of(INFO_RECORD + r(RecordType.TEXT, 1 << bit, b"OK")), ("flags", len(INFO_RECORD)),
                 f"TEXT flag bit {bit}")
        check_eq(fault_of(r(RecordType.INFO, 1 << bit, b"{}")), ("flags", 0), f"INFO flag bit {bit}")
        check_eq(fault_of(INFO_RECORD + r(0x40, 1 << bit, b"x")), ("flags", len(INFO_RECORD)),
                 f"unknown type flag bit {bit} is not skipped")


def test_rule_chain_cap_4608() -> None:
    k = b"k" * 1024
    four = b"".join(r(RecordType.TEXT, rec.MORE, k) for _ in range(4))
    at_cap = INFO_RECORD + four + r(RecordType.TEXT, 0, b"z" * 512)
    evs = events_of(at_cap)
    check(isinstance(evs[-1], Text) and len(evs[-1].data) == 4608, "a 4608-byte chain ending without MORE")
    check_eq(fault_of(INFO_RECORD + four + r(RecordType.TEXT, rec.MORE, b"z" * 512)), ("cap", len(INFO_RECORD) + 4 * 1028),
             "MORE at exactly 4608")
    check_eq(fault_of(INFO_RECORD + four + r(RecordType.TEXT, 0, b"z" * 513)), ("cap", len(INFO_RECORD) + 4 * 1028),
             "4609")
    check_eq(fault_of(INFO_RECORD + four + r(RecordType.TEXT, rec.MORE, b"z" * 511) + r(RecordType.TEXT, 0, b"!")),
             None, "MORE at 4607 then one byte")
    blob = INFO_RECORD + b"".join(r(RecordType.BLOB, rec.MORE, k) for _ in range(4)) + r(RecordType.BLOB, rec.MORE,
                                                                                         b"z" * 512)
    check_eq(fault_of(blob), ("cap", len(INFO_RECORD) + 4 * 1028), "BLOB MORE at 4608")


def test_rule_interleave() -> None:
    open_text = INFO_RECORD + r(RecordType.TEXT, rec.MORE, b"t")
    open_blob = INFO_RECORD + r(RecordType.BLOB, rec.MORE, b"b")
    cases = [(open_text, r(RecordType.BLOB, 0, b"x"), "interleave"), (open_text, r(RecordType.CARD, 0, CARD), "interleave"),
             (open_text, r(RecordType.MIRROR, 0, b"m"), "interleave"), (open_blob, r(RecordType.TEXT, 0, b"x"), "interleave"),
             (open_blob, INFO_RECORD, "info-order")]
    for start, intruder, fault in cases:
        check_eq(fault_of(start + intruder), (fault, len(start)), f"{intruder[:1].hex()} inside a chain")


def test_rule_abort() -> None:
    evs = events_of(INFO_RECORD, r(RecordType.TEXT, rec.MORE, b"part"), r(RecordType.ABORT), r(RecordType.TEXT, 0, b"OK"))
    check_eq(evs[1:], [Abort(RecordType.TEXT, b"part"), Text(b"OK")], "ABORT cuts the chain, the stream goes on")
    evs = events_of(INFO_RECORD, r(RecordType.BLOB, rec.MORE, b"\x00\x01"), r(RecordType.ABORT))
    check_eq(evs[1:], [Abort(RecordType.BLOB, b"\x00\x01")], "ABORT cuts a BLOB chain")
    e = ConsoleEndpoint()
    evs = events_of(INFO_RECORD, r(RecordType.ABORT), r(RecordType.ABORT), r(RecordType.TEXT, 0, b"OK"), endpoint=e)
    check_eq(evs[1:], [Text(b"OK")], "a stray ABORT is ignored")
    check_eq((e.stats.ignored, e.stats.aborts), (2, 0), "stray ABORTs counted as ignored")
    check_eq(fault_of(INFO_RECORD, r(RecordType.TEXT, rec.MORE, b"a") + r(RecordType.ABORT, 0, b"x")),
             ("length", len(INFO_RECORD) + 5), "ABORT with a payload")


def test_rule_skip_unknown_and_ticket_even_in_a_chain() -> None:
    e = ConsoleEndpoint()
    evs = events_of(INFO_RECORD, r(RecordType.TEXT, rec.MORE, b"he"), r(0x40, 0, b"abc"), r(RecordType.TICKET, 0, b"t"),
                    r(0xFF), r(RecordType.TEXT, 0, b"llo"), endpoint=e)
    check_eq(evs[1:], [Text(b"hello")], "the chain goes on across skipped records")
    check_eq(e.stats.skipped, 3, "skipped counted")
    check_eq(fault_of(INFO_RECORD + r(0x40, 0, bytes(1025))), ("length", len(INFO_RECORD)), "unknown over 1024")
    check_eq(fault_of(INFO_RECORD + r(0x40, 0, bytes(1024)) + r(RecordType.TEXT, 0, b"x")), None, "unknown at 1024")


def test_rule_direction() -> None:
    for rtype, payload in ((RecordType.COMMAND, b"help"), (RecordType.BINARY, rec.ota_frame(0, b"\x90")),
                           (RecordType.RESUME, bytes(16))):
        check_eq(fault_of(INFO_RECORD + r(rtype, 0, payload)), ("direction", len(INFO_RECORD)),
                 f"{rec.type_name(rtype)} at the phone")
    check_eq(fault_of(r(RecordType.COMMAND, 0, b"help")), ("direction", 0), "direction before INFO")


def test_rule_lengths() -> None:
    for rtype, top in ((RecordType.TEXT, 1024), (RecordType.BLOB, 1024), (RecordType.CARD, 512), (RecordType.MIRROR, 512)):
        check_eq(fault_of(INFO_RECORD + r(rtype, 0, b"")), ("length", len(INFO_RECORD)), f"{rtype.name} 0")
        check_eq(fault_of(INFO_RECORD + r(rtype, 0, b"x" * (top + 1))), ("length", len(INFO_RECORD)), f"{rtype.name} over")
        check_eq(fault_of(INFO_RECORD + r(rtype, 0, b"x" * top)), None, f"{rtype.name} at {top}")
    check_eq(fault_of(r(RecordType.INFO, 0, b"")), ("length", 0), "INFO 0")
    check_eq(fault_of(r(RecordType.INFO, 0, b"x" * 257)), ("length", 0), "INFO 257")
    check_eq(fault_of(r(RecordType.INFO, 0, b"x" * 256)), None, "INFO 256")


# --------------------------------------------------------------------------------------------
# Endpoint events
# --------------------------------------------------------------------------------------------

def endpoint_events(stream: bytes, chunk: int) -> tuple[ConsoleEndpoint, list[Any]]:
    e = ConsoleEndpoint()
    out: list[Any] = []
    for i in range(0, len(stream), chunk):
        out += e.feed(stream[i:i + chunk])
    return e, out


def test_endpoint_blob_pairs_with_its_raw_header() -> None:
    by_id = {v["id"]: v for v in x4()["vectors"]}
    for chunk in (1, 7, 244, 5000):
        _, evs = endpoint_events(stream_of(by_id["blob_then_raw_header"]), chunk)
        check_eq([type(e).__name__ for e in evs], ["Info", "Blob"], f"blob_then_raw_header /{chunk}")
        blob = evs[1]
        check(blob.data == pattern(1300, 0x42) and blob.header == raw_header(5000, 0, 1300, False), "paired bytes")
        check(blob.length_matches and blob.header_fields["enc"] == "raw", "header fields")

        _, evs = endpoint_events(stream_of(by_id["blob_output_then_header"]), chunk)
        check_eq([type(e).__name__ for e in evs], ["Info", "Text", "Blob"], f"output TEXT passes /{chunk}")
        check(evs[1].text == "synthetic output line\n" and evs[2].data == pattern(700, 0x43), "order and bytes")

        e, evs = endpoint_events(stream_of(by_id["blob_4608"]), chunk)
        check(isinstance(evs[-1], Blob) and len(evs[-1].data) == 4608 and evs[-1].length_matches, "4608 BLOB")
        check(e.pending_blob is None and e.flush() == [], "nothing left")


def test_endpoint_blob_edge_cases() -> None:
    header = raw_header(10, 10, 0, True)
    evs = events_of(INFO_RECORD, r(RecordType.TEXT, 0, header))
    check_eq(evs[1:], [Blob(b"", header)], "an empty window: header alone")
    e = ConsoleEndpoint()
    evs = events_of(INFO_RECORD, r(RecordType.BLOB, 0, b"one"), r(RecordType.BLOB, 0, b"two"), endpoint=e)
    check_eq(evs[1:], [Blob(b"one", None)], "a BLOB superseded before its header is an orphan")
    check_eq(e.pending_blob, b"two", "the newer one waits")
    check_eq(e.flush(), [Blob(b"two", None)], "flush hands on the orphan")
    check_eq(e.stats.orphan_blobs, 2, "orphans counted")
    evs = events_of(INFO_RECORD, r(RecordType.BLOB, 0, b"b"), r(RecordType.CARD, 0, CARD), r(RecordType.MIRROR, 0, b"m"),
                    r(RecordType.TEXT, 0, b'{"enc":"utf8"}'), r(RecordType.TEXT, 0, header))
    check_eq(evs[1:], [Card(CARD), Mirror(b"m"), Text(b'{"enc":"utf8"}'), Blob(b"b", header)],
             "CARD, MIRROR and a non-raw JSON TEXT pass before the header")
    check(not rec.is_raw_header(b'{"enc":"raw"') and not rec.is_raw_header(b"enc raw"), "not JSON")


def test_endpoint_events_for_every_ok_vector() -> None:
    """Every ok p4_to_phone vector, at several splits: the events follow from its deliveries."""
    for v in vectors("p4_to_phone"):
        if v["verdict"] != "ok":
            continue
        want: list[Any] = []
        pending: bytes | None = None
        for d in v["deliveries"]:
            data = bytes.fromhex(d["hex"])
            match d["kind"]:
                case "info": want.append(Info(data))
                case "text" if rec.is_raw_header(data):
                    want.append(Blob(pending or b"", data))
                    pending = None
                case "text": want.append(Text(data))
                case "blob": pending = data
                case "card": want.append(Card(data))
                case "mirror": want.append(Mirror(data))
                case "abort": want.append(Abort(RecordType.TEXT, data))
        for chunk in (1, 13, 244, 100000):
            e, evs = endpoint_events(stream_of(v), chunk)
            check_eq(evs, want, f"{v['id']} /{chunk}: events")
            check(not e.failed and e.pending_blob is None and e.buffered == 0, f"{v['id']}: clean end")
            check_eq((e.stats.skipped, e.stats.ignored), (v["skipped"], v["ignored"]), f"{v['id']}: counters")
            check(e.info is not None and e.info.fields is not None and e.info.fields["v"] == 1, f"{v['id']}: info")


def test_endpoint_text_decoded_per_chain() -> None:
    word = "naïve ☃ 😀".encode()
    evs = events_of(INFO_RECORD, *(r(RecordType.TEXT, rec.MORE if i < len(word) - 1 else 0, word[i:i + 1])
                                   for i in range(len(word))))
    check_eq(evs[1:], [Text(word)], "one message from one-byte records")
    check(evs[1].utf8_valid and evs[1].text == "naïve ☃ 😀", "decoded after assembly")
    bad = Text(b"\xff")
    check(not bad.utf8_valid and bad.text == "�", "invalid UTF-8 replaced")


def test_endpoint_malformed_latches_and_reset_starts_over() -> None:
    e = ConsoleEndpoint()
    evs = e.feed(INFO_RECORD + r(RecordType.TEXT, rec.MORE, b"a") + r(RecordType.CARD, 0, CARD)[:4])
    check(isinstance(evs[-1], Malformed) and evs[-1].fault is Fault.INTERLEAVE, "interleave shows on the header")
    check(e.failed and e.malformed is evs[-1], "latched")
    check("interleave at offset" in evs[-1].reason and "CARD" in evs[-1].reason, "reason")
    check_eq(e.feed(r(RecordType.TEXT, 0, b"OK")), [], "later bytes dropped")
    e.reset()
    check(not e.failed and e.info is None and e.stats.bytes_in == 0, "reset")
    evs = e.feed(INFO_RECORD + r(RecordType.TEXT, 0, b"OK"))
    check_eq([type(x).__name__ for x in evs], ["Info", "Text"], "a fresh stream works")


def test_endpoint_partial_record_is_buffered() -> None:
    e = ConsoleEndpoint()
    record = INFO_RECORD + r(RecordType.TEXT, 0, b"hello")
    check_eq(e.feed(record[:-2]), [Info(INFO_RECORD[4:])], "INFO out, TEXT waits")
    check_eq(e.buffered, 7, "partial TEXT held")
    check_eq(e.feed(record[-2:]), [Text(b"hello")], "completed")
    check_eq(e.buffered, 0, "buffer empty")


def test_endpoint_builders() -> None:
    e = ConsoleEndpoint()
    check_eq(e.command("help"), bytes.fromhex("8100040068656c70"), "command")
    check_eq(e.binary(rec.ota_frame(0, b"\x90")), bytes.fromhex("82000d00004857314f5441010000000090"), "binary")
    raises(RecordError, e.command, "two\nlines")
    check_eq((e.stats.commands_built, e.stats.binaries_built), (1, 1), "counted when built")


# --------------------------------------------------------------------------------------------
# P4 writer (Egress) and a random walk: writer output always passes the phone's receiver
# --------------------------------------------------------------------------------------------

def test_egress_rules() -> None:
    g = Egress()
    check_eq(g.check(RecordType.ABORT, 0, 0), Fault.STRAY_ABORT, "stray abort")
    check_eq(g.check(RecordType.INFO, 0, 10), Fault.TYPE, "INFO is not a writer record")
    check_eq(g.check(RecordType.TICKET, 0, 10), Fault.TYPE, "TICKET")
    check_eq(g.check(RecordType.CARD, rec.MORE, 10), Fault.MORE, "MORE on CARD")
    check_eq(g.check(RecordType.TEXT, 0x02, 10), Fault.FLAGS, "flag bit 1")
    check_eq(g.check(RecordType.TEXT, 0, 1025), Fault.LENGTH, "length")
    for _ in range(4):
        check(g.commit(RecordType.TEXT, rec.MORE, 1024), "4 x 1024 with MORE")
    check_eq(g.check(RecordType.CARD, 0, 10), Fault.INTERLEAVE, "CARD in a chain")
    check_eq(g.check(RecordType.TEXT, rec.MORE, 512), Fault.CAP, "MORE at 4608")
    check_eq(g.check(RecordType.TEXT, 0, 513), Fault.CAP, "4609")
    check_eq(g.check(RecordType.TEXT, 0, 512), Fault.NONE, "4608")
    check(g.commit(RecordType.ABORT, 0, 0) and not g.chain_open, "ABORT closes")
    check_eq((rec.slice_len(1025), rec.slice_flags(1025), rec.slice_len(7), rec.slice_flags(1024)),
             (1024, rec.MORE, 7, 0), "slicing")
    check(rec.chain_fits(4608) and not rec.chain_fits(4609) and not rec.chain_fits(0), "chain_fits")


def test_random_walk_writer_and_receiver_agree() -> None:
    rng = random.Random(9)
    for _ in range(60):
        w = P4Writer()
        w.info(FW, MODEL)
        want: list[Any] = [Info(rec.build_info(FW, MODEL))]
        for _ in range(rng.randint(1, 12)):
            choice = rng.random()
            if choice < 0.5:
                data = bytes(rng.randrange(256) for _ in range(rng.randint(1, rec.CHAIN_MAX)))
                rtype = rng.choice((RecordType.TEXT, RecordType.TEXT, RecordType.BLOB))
                records = -(-len(data) // 1024)
                cut = rng.randrange(1, records) if records > 1 and rng.random() < 0.3 else None
                w.chain(rtype, data, cut_after=cut)
                if cut is not None:
                    want.append(Abort(rtype, data[:cut * 1024]))
                elif rtype == RecordType.TEXT and not rec.is_raw_header(data):
                    want.append(Text(data))
                elif rtype == RecordType.BLOB:
                    want.append(Blob(data, None))  # flushed or superseded: no header follows here
            elif choice < 0.75:
                card = bytes(rng.randrange(32, 127) for _ in range(rng.randint(1, 512)))
                w.single(RecordType.CARD, card)
                want.append(Card(card))
            else:
                text = bytes(rng.randrange(32, 127) for _ in range(rng.randint(1, 1500)))
                w.single(RecordType.MIRROR, text)
                want += [Mirror(text[i:i + 512]) for i in range(0, len(text), 512)]
        stream = bytes(w.stream)
        e = ConsoleEndpoint()
        got: list[Any] = []
        offset = 0
        while offset < len(stream):
            n = rng.randint(1, 600)
            got += e.feed(stream[offset:offset + n])
            offset += n
        got += e.flush()
        check(not e.failed, "writer output is never malformed")
        # Orphan BLOBs come out when superseded or flushed, so compare as multisets of kinds plus
        # the ordered non-BLOB events.
        check_eq([x for x in got if not isinstance(x, Blob)], [x for x in want if not isinstance(x, Blob)],
                 "non-BLOB events in order")
        check_eq([x.data for x in got if isinstance(x, Blob)], [x.data for x in want if isinstance(x, Blob)],
                 "BLOB data in order")
