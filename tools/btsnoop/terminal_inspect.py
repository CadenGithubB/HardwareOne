#!/usr/bin/env python3
"""Strict, offline SID 0x30 framing inspection; no Terminal schema is assumed.

Only H4 btsnoop v1, fixed-channel ATT writes/notifications/indications and
G2 AA21/AA12 envelopes are supported. Captures must contain complete ACL and
selected G2 messages: missing starts, dropped records and partial tails fail.
Length-delimited protobuf fields remain opaque (not guessed to be submessages).
No files are extracted or written, and payload bytes are private by default.
"""

from __future__ import annotations

import argparse
import binascii
import hashlib
import json
import struct
import sys
import zipfile
import zlib
from pathlib import Path

MAX_INPUT = 64 * 1024 * 1024
MAX_ARCHIVE = 512 * 1024 * 1024
MAX_ZIP_ENTRIES = 4096
MAX_ZIP_DIRECTORY = 2 * 1024 * 1024
MAX_RECORDS = 1_000_000
MAX_RECORD_BYTES = 65540
MAX_PENDING = 64
MAX_L2CAP = 65539  # u16 payload length plus four-byte header
MAX_G2 = 65535
MAX_MESSAGES = 10000
MAX_FIELDS = 4096
MAX_TOTAL_FIELDS = 100000
TERMINAL_SID = 0x30
ATT_VALUE_OPS = frozenset((0x12, 0x52, 0x1B, 0x1D))


class CaptureError(ValueError):
    """A malformed, incomplete or unsupported capture; never includes payload."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise CaptureError(reason)


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def preflight_zip(source) -> int:
    """Bound ZipFile's eager directory read; deliberately support classic ZIP only."""
    size = source.seek(0, 2)
    require(size <= MAX_ARCHIVE, "ZIP exceeds 512 MiB archive limit")
    # EOCD is 22 bytes plus at most 65535 comment bytes. Include another 20
    # bytes to detect a ZIP64 locator even with the maximum-length comment.
    tail_start = max(0, size - (22 + 65535 + 20))
    source.seek(tail_start)
    tail = source.read(size - tail_start)
    # ZipFile selects the last signature too. Do not skip a malformed later
    # candidate and accidentally preflight a different directory than it reads.
    offset = tail.rfind(b"PK\x05\x06")
    require(offset >= 0 and offset + 22 <= len(tail),
            "missing or truncated classic ZIP end record")
    fields = struct.unpack_from("<4H2IH", tail, offset + 4)
    require(offset + 22 + fields[-1] == len(tail), "invalid ZIP end record/comment length")
    disk, directory_disk, disk_entries, entries, directory_size, directory_offset, _ = fields
    require(disk == 0 and directory_disk == 0 and disk_entries == entries,
            "multidisk ZIP is unsupported")
    require(entries != 0xFFFF and directory_size != 0xFFFFFFFF
            and directory_offset != 0xFFFFFFFF
            and (offset < 20 or tail[offset - 20:offset - 16] != b"PK\x06\x07"),
            "ZIP64 is unsupported")
    require(entries <= MAX_ZIP_ENTRIES, "too many ZIP entries")
    require(directory_size <= MAX_ZIP_DIRECTORY, "ZIP central directory exceeds 2 MiB limit")
    require(directory_size >= entries * 46, "invalid ZIP central directory size")
    require(directory_offset + directory_size == tail_start + offset,
            "unsupported ZIP central directory layout")
    source.seek(0)
    return entries


def list_logs(path: Path) -> dict:
    """List only candidate log metadata; never open any ZIP member."""
    try:
        with path.open("rb") as source:
            expected_entries = preflight_zip(source)
            with zipfile.ZipFile(source) as archive:
                entries = archive.infolist()
                require(len(entries) == expected_entries, "ZIP entry count mismatch")
                logs = [{"member": entry.filename, "size_bytes": entry.file_size,
                         "compressed_bytes": entry.compress_size}
                        for entry in entries if not entry.is_dir()
                        and entry.filename.rsplit("/", 1)[-1] in
                        ("btsnoop_hci.log", "btsnoop_hci.log.last")]
                return {"schema": 1, "logs": logs}
    except (OSError, zipfile.BadZipFile) as exc:
        raise CaptureError("could not read ZIP directory") from exc


def load_capture(path: Path, member: str | None = None) -> bytes:
    """Read a bounded log, or only an explicitly selected ZIP member in memory."""
    try:
        with path.open("rb") as source:
            size = source.seek(0, 2)
            source.seek(0)
            if member is None:
                require(size <= MAX_INPUT, "input exceeds 64 MiB log limit")
                data = source.read(MAX_INPUT + 1)
            else:
                expected_entries = preflight_zip(source)
                with zipfile.ZipFile(source) as archive:
                    entries = archive.infolist()
                    require(len(entries) == expected_entries, "ZIP entry count mismatch")
                    matches = [entry for entry in entries if entry.filename == member]
                    require(len(matches) == 1, "ZIP member must identify exactly one entry")
                    entry = matches[0]
                    require(not entry.is_dir(), "ZIP member is a directory")
                    require(not entry.flag_bits & 1, "encrypted ZIP member is unsupported")
                    require(entry.compress_type in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED),
                            "ZIP compression method is unsupported")
                    require(entry.file_size <= MAX_INPUT, "ZIP member exceeds 64 MiB log limit")
                    with archive.open(entry) as log:
                        data = log.read(MAX_INPUT + 1)
                    require(len(data) == entry.file_size, "ZIP member length mismatch")
            require(len(data) <= MAX_INPUT, "input exceeds 64 MiB log limit")
            return data
    except (OSError, zipfile.BadZipFile, RuntimeError, NotImplementedError,
            EOFError, zlib.error) as exc:
        # ZIP/OS exceptions can contain private member paths; do not echo them.
        raise CaptureError("could not read input or ZIP member") from exc


def records(data: bytes):
    require(len(data) <= MAX_INPUT, "input exceeds 64 MiB log limit")
    require(len(data) >= 16 and data[:8] == b"btsnoop\0", "expected btsnoop header")
    require(struct.unpack_from(">II", data, 8) == (1, 1002),
            "only btsnoop version 1 / H4 datalink 1002 is supported")
    offset = 16
    record = 0
    while offset < len(data):
        require(record < MAX_RECORDS, "record count limit exceeded")
        require(len(data) - offset >= 24, "truncated btsnoop record header")
        original, included, flags, drops, timestamp = struct.unpack_from(">IIIIQ", data, offset)
        offset += 24
        require(original == included, "truncated btsnoop packet")
        require(0 < included <= MAX_RECORD_BYTES, "invalid btsnoop packet length")
        require(included <= len(data) - offset, "truncated btsnoop record body")
        require(flags <= 3 and drops == 0, "unsupported flags or capture reports dropped packets")
        record += 1
        yield record, timestamp, flags & 1, data[offset:offset + included]
        offset += included


def varint(data: bytes, offset: int) -> tuple[int, int]:
    value = 0
    for index in range(10):
        require(offset < len(data), "truncated protobuf varint")
        byte = data[offset]
        offset += 1
        require(index != 9 or byte <= 1, "protobuf varint overflow")
        value |= (byte & 0x7F) << (7 * index)
        if byte < 0x80:
            return value, offset
    raise CaptureError("protobuf varint overflow")


def protobuf_fields(data: bytes) -> list[dict]:
    """Top-level wire shape only. Numeric values are unsigned, without enums."""
    fields = []
    offset = 0
    while offset < len(data):
        require(len(fields) < MAX_FIELDS, "protobuf field count limit exceeded")
        start = offset
        tag, offset = varint(data, offset)
        number, wire = tag >> 3, tag & 7
        require(0 < number < (1 << 29), "invalid protobuf field number")
        item = {"field": number, "wire_type": wire}
        if wire == 0:
            value_start = offset
            item["unsigned_value"], offset = varint(data, offset)
            item["value_length"] = offset - value_start
        elif wire in (1, 5):
            length = 8 if wire == 1 else 4
            require(length <= len(data) - offset, "truncated protobuf fixed field")
            item["value_length"] = length
            item["unsigned_value"] = int.from_bytes(data[offset:offset + length], "little")
            offset += length
        elif wire == 2:
            length, offset = varint(data, offset)
            require(length <= len(data) - offset, "truncated protobuf length-delimited field")
            item["value_length"] = length
            offset += length
        else:
            raise CaptureError("unsupported protobuf wire type (groups are not decoded)")
        item["encoded_length"] = offset - start
        fields.append(item)
    return fields


class Inspector:
    def __init__(self, include_payload: bool):
        self.include_payload = include_payload
        self.acl = {}  # (connection handle, direction) -> complete L2CAP assembly
        self.g2 = {}  # (connection handle, ATT handle, direction, seq, SID)
        self.epochs = {}  # handle reuse after a successful disconnect
        self.messages = []
        self.field_count = 0
        self.record_count = 0

    def h4(self, record: int, timestamp: int, direction: int, packet: bytes) -> None:
        kind = packet[0]
        if kind == 2:
            require(len(packet) >= 5, "truncated H4 ACL header")
            handle_flags, length = struct.unpack_from("<HH", packet, 1)
            require(len(packet) == 5 + length, "H4 ACL length mismatch")
            require(handle_flags >> 14 == 0, "broadcast ACL is unsupported")
            handle, boundary = handle_flags & 0xFFF, (handle_flags >> 12) & 3
            self.acl_fragment(record, timestamp, direction, handle, boundary, packet[5:])
            return
        # Validate other H4 packet lengths without inspecting their payloads.
        require(kind in (1, 3, 4, 5), "unsupported H4 packet type")
        header_length = {1: 4, 3: 4, 4: 3, 5: 5}[kind]
        require(len(packet) >= header_length, "truncated H4 header")
        length = (struct.unpack_from("<H", packet, 3)[0] & 0x3FFF
                  if kind == 5 else packet[header_length - 1])
        require(len(packet) == header_length + length, "H4 packet length mismatch")
        if kind == 4 and packet[1] == 5:  # Disconnection Complete, not a G2 enum.
            require(length == 4, "invalid HCI disconnect event length")
            if packet[3] == 0:
                handle = struct.unpack_from("<H", packet, 4)[0]
                require(not any(key[0] == handle for key in (*self.acl, *self.g2)),
                        "disconnect interrupted an incomplete message")
                self.epochs[handle] = self.epochs.get(handle, 0) + 1

    def acl_fragment(self, record: int, timestamp: int, direction: int,
                     handle: int, boundary: int, fragment: bytes) -> None:
        key = handle, direction
        if boundary != 1:
            require(key not in self.acl, "new ACL start before prior message completed")
            require(len(fragment) >= 4, "truncated L2CAP header")
            length, cid = struct.unpack_from("<HH", fragment)
            require(length + 4 <= MAX_L2CAP, "L2CAP reassembly limit exceeded")
            require(len(self.acl) < MAX_PENDING, "too many pending ACL messages")
            state = {"length": length + 4, "cid": cid, "bytes": bytearray(),
                     "record": record, "timestamp": timestamp, "count": 0}
            self.acl[key] = state
        else:
            require(key in self.acl, "ACL continuation without a start on this connection/direction")
            state = self.acl[key]
        require(bool(fragment), "empty ACL fragment")
        require(len(fragment) <= state["length"] - len(state["bytes"]),
                "ACL fragments exceed declared L2CAP length")
        state["bytes"].extend(fragment)
        state["count"] += 1
        require(boundary != 3 or len(state["bytes"]) == state["length"],
                "complete ACL packet contains incomplete L2CAP data")
        if len(state["bytes"]) == state["length"]:
            del self.acl[key]
            if state["cid"] == 4:
                self.att(handle, direction, state, record)

    def att(self, connection: int, direction: int, state: dict, last_record: int) -> None:
        att = bytes(state["bytes"][4:])
        require(bool(att), "empty ATT packet")
        if att[0] not in ATT_VALUE_OPS:
            return
        require(len(att) >= 3, "truncated ATT value header")
        handle = struct.unpack_from("<H", att, 1)[0]
        require(handle != 0, "invalid ATT handle")
        value = att[3:]
        if value[:2] not in (b"\xaa\x21", b"\xaa\x12"):
            return
        require(len(value) >= 8, "truncated G2 envelope header")
        if value[6] != TERMINAL_SID:
            return  # Never decode or emit another service's data.
        require(value[1] in (0x21, 0x12), "unsupported G2 direction marker")
        seq, declared, count, index, sid, flag = value[2:8]
        require(len(value) == 8 + declared, "G2 declared length mismatch")
        require(0 < index <= count, "invalid G2 fragment index/count")
        final = index == count
        require(not final or declared >= 2, "missing G2 final CRC")
        part = value[8:-2] if final else value[8:]
        key = connection, handle, direction, seq, sid
        if index == 1:
            require(key not in self.g2, "new G2 start before prior message completed")
            require(len(self.g2) < MAX_PENDING, "too many pending G2 messages")
            self.g2[key] = {"count": count, "next": 1, "flag": flag,
                            "preamble": value[1], "bytes": bytearray(), "fragments": []}
        require(key in self.g2, "G2 continuation without a start on this connection/handle/direction")
        assembly = self.g2[key]
        require(index == assembly["next"] and count == assembly["count"]
                and flag == assembly["flag"] and value[1] == assembly["preamble"],
                "G2 fragment order/count/flags changed")
        require(len(part) <= MAX_G2 - len(assembly["bytes"]), "G2 reassembly limit exceeded")
        assembly["bytes"].extend(part)
        assembly["next"] += 1
        assembly["fragments"].append({
            "index": index, "first_record": state["record"], "last_record": last_record,
            "btsnoop_timestamp_us": state["timestamp"], "acl_fragments": state["count"],
            "l2cap_length": state["length"] - 4, "att_opcode": att[0],
            "att_value_length": len(value), "declared_length": declared})
        if final:
            payload = bytes(assembly["bytes"])
            require(binascii.crc_hqx(payload, 0xFFFF) == int.from_bytes(value[-2:], "little"),
                    "G2 payload CRC mismatch")
            fields = protobuf_fields(payload)
            self.field_count += len(fields)
            require(self.field_count <= MAX_TOTAL_FIELDS, "total protobuf field limit exceeded")
            require(len(self.messages) < MAX_MESSAGES, "message count limit exceeded")
            message = {"connection_handle": connection,
                       "connection_epoch": self.epochs.get(connection, 0),
                       "att_handle": handle, "direction": direction,
                       "g2_direction_marker": value[1], "sequence": seq, "sid": sid,
                       "flag": flag, "fragment_count": count, "crc_valid": True,
                       "payload_length": len(payload), "payload_sha256": sha256(payload),
                       "fields": fields, "fragments": assembly["fragments"]}
            if self.include_payload:
                message["payload_hex"] = payload.hex()
            self.messages.append(message)
            del self.g2[key]


def inspect_capture(data: bytes, include_payload: bool = False) -> dict:
    inspector = Inspector(include_payload)
    for record, timestamp, direction, packet in records(data):
        inspector.record_count = record
        try:
            inspector.h4(record, timestamp, direction, packet)
        except CaptureError as exc:
            raise CaptureError(f"record {record}: {exc}") from exc
    require(not inspector.acl, "capture ends with incomplete ACL reassembly")
    require(not inspector.g2, "capture ends with incomplete SID 0x30 reassembly")
    return {"schema": 1, "sid": TERMINAL_SID, "input_bytes": len(data),
            "input_sha256": sha256(data), "records": inspector.record_count,
            "payload_included": include_payload, "messages": inspector.messages}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path, help="H4 btsnoop log or bugreport ZIP")
    selection = parser.add_mutually_exclusive_group()
    selection.add_argument("--zip-member", help="exact log member name; only this entry is read")
    selection.add_argument("--list-logs", action="store_true",
                           help="list btsnoop_hci.log and .log.last member names/sizes only")
    parser.add_argument("--include-payload", action="store_true",
                        help="include private complete protobuf payload hex in JSON")
    args = parser.parse_args(argv)
    try:
        report = (list_logs(args.capture) if args.list_logs else
                  inspect_capture(load_capture(args.capture, args.zip_member), args.include_payload))
    except CaptureError as exc:
        print(f"terminal_inspect: {exc}", file=sys.stderr)
        return 2
    # Emit only after the entire capture has passed; no misleading partial result.
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
