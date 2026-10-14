#!/usr/bin/env python3
"""Small offline fixture suite for the production capture inspector."""

import binascii
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

import terminal_inspect as tool


def capture(packets):
    result = bytearray(b"btsnoop\0" + struct.pack(">II", 1, 1002))
    for index, (packet, direction) in enumerate(packets):
        result += struct.pack(">IIIIQ", len(packet), len(packet), direction, 0, 1000 + index)
        result += packet
    return bytes(result)


def protobuf(text):
    data = text.encode("utf-8")
    length = len(data)
    encoded_length = bytearray()
    while length >= 128:
        encoded_length.append((length & 127) | 128)
        length >>= 7
    encoded_length.append(length)
    return b"\x08\x07\x12" + bytes(encoded_length) + data


def g2_frames(payload, count=2, sid=0x30, marker=0x21):
    # G2 uses a CRC over the whole protobuf, only in the final fragment;
    # declared length includes that CRC. This is the firmware's documented wire format.
    split = (len(payload) + count - 1) // count
    chunks = [payload[index * split:(index + 1) * split] for index in range(count)]
    chunks[-1] += struct.pack("<H", binascii.crc_hqx(payload, 0xFFFF))
    return [bytes((0xAA, marker, 5, len(chunk), count, index + 1, sid, 4)) + chunk
            for index, chunk in enumerate(chunks)]


def acl_packet(part, connection=1, boundary=2, direction=0):
    return b"\x02" + struct.pack("<HH", connection | (boundary << 12), len(part)) + part, direction


def att_packets(value, connection=1, handle=0x40, direction=0, split=True):
    att = bytes((0x52 if direction == 0 else 0x1B,)) + struct.pack("<H", handle) + value
    l2cap = struct.pack("<HH", len(att), 4) + att
    if split:
        return [acl_packet(l2cap[:9], connection, 2, direction),
                acl_packet(l2cap[9:], connection, 1, direction)]
    return [acl_packet(l2cap, connection, 2, direction)]


def packets_for(payload, count=2, **kwargs):
    return [packet for frame in g2_frames(payload, count) for packet in att_packets(frame, **kwargs)]


class TerminalInspectTest(unittest.TestCase):
    def test_fragmented_packet_and_redaction(self):
        self.assertEqual(binascii.crc_hqx(b"123456789", 0xFFFF), 0x29B1)
        private_text = "PRIVATE TERMINAL PROMPT " + "🥔" * 50
        payload = protobuf(private_text)
        data = capture(packets_for(payload, count=3))
        report = tool.inspect_capture(data)
        message, = report["messages"]
        self.assertEqual(report["records"], 6)
        self.assertEqual(message["fragment_count"], 3)
        self.assertEqual([f["acl_fragments"] for f in message["fragments"]], [2, 2, 2])
        self.assertEqual(message["fragments"][0]["btsnoop_timestamp_us"], 1000)
        self.assertEqual(message["payload_sha256"], hashlib.sha256(payload).hexdigest())
        self.assertEqual(message["fields"], [
            {"field": 1, "wire_type": 0, "unsigned_value": 7, "value_length": 1, "encoded_length": 2},
            {"field": 2, "wire_type": 2, "value_length": len(private_text.encode()),
             "encoded_length": len(payload) - 2}])
        public_json = json.dumps(report)
        self.assertNotIn("PRIVATE", public_json)
        self.assertNotIn(payload.hex(), public_json)
        self.assertNotIn("payload_hex", public_json)
        self.assertEqual(tool.inspect_capture(data, True)["messages"][0]["payload_hex"], payload.hex())
        # Other service bodies are neither interpreted as protobuf nor emitted.
        other = capture([packet for f in g2_frames(b"secret-not-protobuf", 1, sid=0x07)
                         for packet in att_packets(f)])
        self.assertEqual(tool.inspect_capture(other)["messages"], [])

    def test_corruption_and_partial_data_are_rejected_without_output(self):
        payload = protobuf("PRIVATE TEST")
        frames = g2_frames(payload)
        packet_list = packets_for(payload)
        valid = capture(packet_list)
        corrupt_crc = frames[-1][:-1] + bytes((frames[-1][-1] ^ 1,))
        wrong_index = frames[-1][:5] + b"\x01" + frames[-1][6:]
        wrong_count = frames[-1][:4] + b"\x03" + frames[-1][5:]
        wrong_length = frames[-1][:3] + b"\xFF" + frames[-1][4:]
        cases = {
            "record header": valid + b"\0",
            "record body": valid[:-1],
            "ACL missing start": capture(packet_list[1:]),
            "ACL incomplete": capture(packet_list[:1]),
            "G2 incomplete": capture(packet_list[:2]),
            "G2 reversed": capture([p for f in reversed(frames) for p in att_packets(f)]),
            "CRC": capture(att_packets(frames[0]) + att_packets(corrupt_crc)),
            "index": capture(att_packets(frames[0]) + att_packets(wrong_index)),
            "count": capture(att_packets(frames[0]) + att_packets(wrong_count)),
            "declared length": capture(att_packets(frames[0]) + att_packets(wrong_length)),
            "protobuf length": capture(packets_for(b"\x12\x7fshort", count=1)),
        }
        for name, data in cases.items():
            with self.subTest(name=name), self.assertRaises(tool.CaptureError):
                tool.inspect_capture(data)
        with patch.object(tool, "MAX_RECORDS", 1), self.assertRaises(tool.CaptureError):
            tool.inspect_capture(valid)
        with tempfile.TemporaryDirectory() as temp:
            source = Path(temp) / "capture.log"
            source.write_bytes(cases["CRC"])
            stdout, stderr = io.StringIO(), io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                self.assertEqual(tool.main([str(source)]), 2)
            self.assertEqual(stdout.getvalue(), "")
            self.assertIn("CRC mismatch", stderr.getvalue())
            self.assertNotIn("PRIVATE", stderr.getvalue())

    def test_connection_att_handle_direction_and_reuse_isolation(self):
        # Same sequence/SID on four independent flows, interleaved at BOTH layers.
        flows = [(1, 0x40, 0), (2, 0x40, 0), (1, 0x41, 0), (1, 0x40, 1)]
        payloads = [protobuf(f"connection message {index}") for index in range(len(flows))]
        frames = [g2_frames(p, marker=0x12 if flow[2] else 0x21)
                  for p, flow in zip(payloads, flows)]
        packets = []
        # First fragments on the same connection/direction complete before the
        # next ATT handle starts: L2CAP itself has no ATT handle routing yet.
        for index in range(2):
            for chosen in ((0, 1, 3), (2,)):
                pairs = [att_packets(frames[n][index], *flows[n]) for n in chosen]
                packets += [pair[0] for pair in pairs] + [pair[1] for pair in pairs]
        messages = tool.inspect_capture(capture(packets), True)["messages"]
        actual = {(m["connection_handle"], m["att_handle"], m["direction"]): m["payload_hex"]
                  for m in messages}
        self.assertEqual(actual, {flow: p.hex() for flow, p in zip(flows, payloads)})
        for connection, handle, direction in ((2, 0x40, 0), (1, 0x41, 0), (1, 0x40, 1)):
            with self.subTest(connection=connection, handle=handle, direction=direction):
                mixed = att_packets(frames[0][0]) + att_packets(frames[0][1], connection, handle, direction)
                with self.assertRaises(tool.CaptureError):
                    tool.inspect_capture(capture(mixed))
        disconnect = (b"\x04\x05\x04\x00\x01\x00\x13", 1)
        complete = packets_for(payloads[0], count=1)
        reused = tool.inspect_capture(capture(complete + [disconnect] + complete))
        self.assertEqual([m["connection_epoch"] for m in reused["messages"]], [0, 1])

    def test_zip_selects_one_bounded_member_in_memory(self):
        valid = capture(packets_for(protobuf("PRIVATE ZIP TEXT")))
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "bugreport.zip"
            member = "FS/data/misc/bluetooth/logs/btsnoop_hci.log"
            with zipfile.ZipFile(source, "w", zipfile.ZIP_DEFLATED) as archive:
                archive.writestr("unrelated/private.txt", b"do not extract")
                archive.writestr(member, valid)
                archive.writestr(member + ".last", b"older log")
            with patch.object(zipfile.ZipFile, "open", side_effect=AssertionError("must not extract")):
                logs = tool.list_logs(source)["logs"]
            self.assertEqual([entry["member"] for entry in logs], [member, member + ".last"])
            self.assertEqual(logs[0]["size_bytes"], len(valid))
            self.assertEqual(tool.load_capture(source, member), valid)
            self.assertEqual(list(root.iterdir()), [source])
            with self.assertRaises(tool.CaptureError):
                tool.load_capture(source, "missing.log")
            with patch.object(tool, "MAX_INPUT", len(valid) - 1), self.assertRaises(tool.CaptureError):
                tool.load_capture(source, member)
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                self.assertEqual(tool.main([str(source), "--zip-member", member]), 0)
            self.assertEqual(len(json.loads(stdout.getvalue())["messages"]), 1)
            self.assertNotIn("PRIVATE", stdout.getvalue())
            # Refuse an oversized eager central-directory read BEFORE ZipFile
            # opens/parses it, through both public ZIP entry points.
            archive_bytes = bytearray(source.read_bytes())
            end_record = archive_bytes.rfind(b"PK\x05\x06")
            struct.pack_into("<I", archive_bytes, end_record + 12, tool.MAX_ZIP_DIRECTORY + 1)
            source.write_bytes(archive_bytes)
            with patch.object(zipfile, "ZipFile", side_effect=AssertionError("must preflight first")):
                for reader in (lambda: tool.list_logs(source), lambda: tool.load_capture(source, member)):
                    with self.assertRaisesRegex(tool.CaptureError, "central directory exceeds"):
                        reader()


if __name__ == "__main__":
    unittest.main()
