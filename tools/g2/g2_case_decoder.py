#!/usr/bin/env python3
"""
Decode G2 glasses-case (sid 0x81) and settings (sid 0x09) frames on the host.

Host-side reference decoder for the labelled-capture phase of
docs/G2_CASE_PRESENCE_POWER_PLAN.md. It is NOT firmware: it exists so the
field mapping can be worked out from captures first, and so the firmware
parser written later has a known-good oracle to agree with.

Input forms (one per line, or as arguments):
  * a bare protobuf payload in hex:        08 01 10 2a 1a 06 08 0c 10 01
  * a full RX envelope in hex:             aa 12 <seq> <len> 01 01 81 00 <pb> <crc lo> <crc hi>
  * a serial-log line that contains hex after "payload=" or "pb=" and a
    "sid=0x.." token. Today the firmware's frame ring (g2RingDump) keeps only
    the first 12 payload bytes per frame; the plan widens that for unparsed
    sids so a whole caseInfo message fits.

Only the stdlib is used. Protobuf wire decoding is hand-rolled because the
schemas are tiny and we want to see unknown fields, not drop them.

Usage:
  python3 tools/g2/g2_case_decoder.py "08 01 10 2a 1a 06 08 0c 10 01"
  python3 tools/g2/g2_case_decoder.py --sid 0x09 "<hex>"
  python3 tools/g2/g2_case_decoder.py --log p4_seriallog.txt
  python3 tools/g2/g2_case_decoder.py --selftest
"""
from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Wire-level helpers
# ---------------------------------------------------------------------------

WIRE_VARINT, WIRE_64, WIRE_LEN, WIRE_32 = 0, 1, 2, 5


def read_varint(buf: bytes, pos: int) -> Tuple[int, int]:
    shift, value = 0, 0
    while True:
        if pos >= len(buf):
            raise ValueError("truncated varint")
        b = buf[pos]
        pos += 1
        value |= (b & 0x7F) << shift
        if not b & 0x80:
            return value, pos
        shift += 7
        if shift > 63:
            raise ValueError("varint too long")


@dataclass
class Field:
    number: int
    wire: int
    value: object  # int for varint, bytes for len-delimited

    def as_int(self) -> Optional[int]:
        return self.value if isinstance(self.value, int) else None


def parse_message(buf: bytes) -> List[Field]:
    """Flat parse of one protobuf message. Nested messages stay as bytes."""
    out: List[Field] = []
    pos = 0
    while pos < len(buf):
        tag, pos = read_varint(buf, pos)
        number, wire = tag >> 3, tag & 7
        if number == 0:
            raise ValueError("field number 0")
        if wire == WIRE_VARINT:
            v, pos = read_varint(buf, pos)
            out.append(Field(number, wire, v))
        elif wire == WIRE_LEN:
            n, pos = read_varint(buf, pos)
            if pos + n > len(buf):
                raise ValueError("truncated length-delimited field")
            out.append(Field(number, wire, bytes(buf[pos:pos + n])))
            pos += n
        elif wire == WIRE_64:
            out.append(Field(number, wire, int.from_bytes(buf[pos:pos + 8], "little")))
            pos += 8
        elif wire == WIRE_32:
            out.append(Field(number, wire, int.from_bytes(buf[pos:pos + 4], "little")))
            pos += 4
        else:
            raise ValueError(f"unsupported wire type {wire}")
    return out


def crc16_ccitt_false(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


# ---------------------------------------------------------------------------
# Envelope
# ---------------------------------------------------------------------------

@dataclass
class Envelope:
    direction: str
    seq: int
    sid: int
    flag: int
    frag_index: int
    frag_total: int
    payload: bytes
    crc_ok: Optional[bool]


def parse_envelope(raw: bytes) -> Envelope:
    """AA 21|12 seq len totFrags fragIdx sid flag <pb> [crc LE on last frag]."""
    if len(raw) < 8 or raw[0] != 0xAA or raw[1] not in (0x21, 0x12):
        raise ValueError("not a G2 envelope")
    direction = "TX" if raw[1] == 0x21 else "RX"
    seq, length, tot, idx, sid, flag = raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]
    body = raw[8:8 + length]
    crc_ok: Optional[bool] = None
    if idx == tot and len(body) >= 2:
        pb, crc = body[:-2], int.from_bytes(body[-2:], "little")
        # CRC-16/CCITT-FALSE over JUST the pb payload, little-endian
        # (G2_PROTOCOL.md "CRC"; System_G2_Protocol.cpp g2BuildEnvelope).
        crc_ok = crc16_ccitt_false(pb) == crc
    else:
        pb = body
    return Envelope(direction, seq, sid, flag, idx, tot, bytes(pb), crc_ok)


# ---------------------------------------------------------------------------
# Schemas (field numbers from the FlutterApp descriptors; see docs/g2_proto)
# ---------------------------------------------------------------------------

CASE_CMD = {0: "NONE_COMMAND", 1: "CASE_INFO"}
CASE_INFO_FIELDS = {1: "soc", 2: "chargeStatus", 3: "lidStatus", 4: "inCaseStatus", 5: "errorCode"}
CASE_PKG_FIELDS = {1: "commandId", 2: "magicRandom", 3: "caseInfo"}

SETTING_CMD = {1: "DeviceReceiveInfo", 2: "DeviceReceiveRequest", 3: "DeviceSendToAPP",
               4: "DeviceRespondToAPP", 5: "AppRespondToDevice"}
SETTING_PKG_FIELDS = {1: "commandId", 2: "magicRandom", 3: "deviceReceiveInfoFromApp",
                      4: "deviceReceiveRequestFromApp", 5: "deviceSendInfoToApp",
                      6: "deviceRespondToApp", 7: "appRespondToDevice"}
# DeviceReceiveRequestFromAPP. The glasses answer a BASIC read by echoing this
# message back in OUTER FIELD 4 with every setting populated (observed on real
# glasses; System_G2_Protocol.cpp). Outer field 6 Device_Respond_To_App is a
# 2-field ack the firmware never emits for reads.
SETTING_REQUEST_FIELDS = {
    1: "settingInfoType", 2: "autoBrightnessLevel", 3: "yCoordinateLevelRestored",
    4: "xCoordinateLevelRestored", 5: "leftSoftwareVersion", 6: "rightSoftwareVersion",
    7: "headUpSwitchRestored", 8: "headUpAngleRestored", 9: "headUpAngleCalibrationRestored",
    10: "wearDetectionSwitchRestored", 11: "deviceRunningStatus", 12: "battery",
    13: "chargingStatus", 14: "silentModeSwitchRestored", 15: "leftCalibrationRestored",
    16: "rightCalibrationRestored", 17: "headUpRecalibrationSuccess",
    18: "autoBrightnessSwitchRestored", 19: "unreadMessageCount",
}
# DeviceSendInfoToAPP (async push). Only two fields are in the community
# schema; anything else that shows up in a capture is exactly what we want.
SETTING_SEND_INFO_FIELDS = {1: "currentRecalibrationStatus", 2: "silentModeSwitch"}
SETTING_RESPOND_FIELDS = {1: "packageId", 2: "receiveFlag"}

# Fields the presence work cares about. Absent-vs-zero caveat applies to all.
PRESENCE_RELEVANT = {"chargeStatus", "lidStatus", "inCaseStatus", "soc",
                     "chargingStatus", "deviceRunningStatus", "wearDetectionSwitchRestored",
                     "battery"}


@dataclass
class Decoded:
    sid: int
    summary: Dict[str, object] = field(default_factory=dict)
    unknown: List[str] = field(default_factory=list)

    def render(self) -> str:
        parts = [f"sid=0x{self.sid:02X}"]
        for k, v in self.summary.items():
            mark = "*" if k.split(".")[-1] in PRESENCE_RELEVANT else ""
            parts.append(f"{k}={v}{mark}")
        if self.unknown:
            parts.append("UNKNOWN[" + ", ".join(self.unknown) + "]")
        return " ".join(parts)


def _decode_named(buf: bytes, names: Dict[int, str], prefix: str, out: Decoded,
                  nested: Optional[Dict[int, Tuple[Dict[int, str], str]]] = None) -> None:
    for f in parse_message(buf):
        name = names.get(f.number)
        key = f"{prefix}{name}" if name else f"{prefix}f{f.number}"
        if f.wire == WIRE_LEN and nested and f.number in nested:
            sub_names, sub_prefix = nested[f.number]
            _decode_named(f.value, sub_names, f"{sub_prefix}.", out)
            continue
        if isinstance(f.value, bytes):
            text = f.value.decode("ascii") if all(32 <= b < 127 for b in f.value) and f.value else f.value.hex()
            value: object = text
        else:
            value = f.value
        if name is None:
            out.unknown.append(f"{key}={value}")
        else:
            out.summary[key] = value


def decode_payload(sid: int, pb: bytes) -> Decoded:
    d = Decoded(sid)
    if sid == 0x81:
        _decode_named(pb, CASE_PKG_FIELDS, "", d, nested={3: (CASE_INFO_FIELDS, "caseInfo")})
        cmd = d.summary.get("commandId")
        if isinstance(cmd, int):
            d.summary["commandId"] = f"{cmd}({CASE_CMD.get(cmd, '?')})"
        # proto3 elision: make the ambiguity explicit in the output.
        if "caseInfo.soc" in d.summary or any(k.startswith("caseInfo.") for k in d.summary):
            for n in ("lidStatus", "inCaseStatus", "chargeStatus"):
                d.summary.setdefault(f"caseInfo.{n}", "absent(=0?)")
    elif sid == 0x09:
        _decode_named(pb, SETTING_PKG_FIELDS, "", d, nested={
            4: (SETTING_REQUEST_FIELDS, "echo"),   # request AND the glasses' full reply
            5: (SETTING_SEND_INFO_FIELDS, "push"),
            6: (SETTING_RESPOND_FIELDS, "ack"),
        })
        cmd = d.summary.get("commandId")
        if isinstance(cmd, int):
            d.summary["commandId"] = f"{cmd}({SETTING_CMD.get(cmd, '?')})"
    else:
        _decode_named(pb, {}, "", d)
    return d


# ---------------------------------------------------------------------------
# Input handling
# ---------------------------------------------------------------------------

HEX_RE = re.compile(r"(?:[0-9A-Fa-f]{2}[\s:,-]*){4,}")
SID_RE = re.compile(r"sid=0x([0-9A-Fa-f]{2})")


def hex_to_bytes(text: str) -> bytes:
    return bytes.fromhex(re.sub(r"[^0-9A-Fa-f]", "", text))


def decode_text(text: str, sid_hint: Optional[int]) -> Optional[Decoded]:
    m = HEX_RE.search(text)
    if not m:
        return None
    raw = hex_to_bytes(m.group(0))
    if len(raw) >= 8 and raw[0] == 0xAA and raw[1] in (0x21, 0x12):
        env = parse_envelope(raw)
        d = decode_payload(env.sid, env.payload)
        d.summary = {"dir": env.direction, "seq": f"0x{env.seq:02X}", "flag": f"0x{env.flag:02X}",
                     "frag": f"{env.frag_index}/{env.frag_total}",
                     "crc": "ok" if env.crc_ok else ("BAD" if env.crc_ok is False else "n/a"),
                     **d.summary}
        return d
    sid = sid_hint
    sm = SID_RE.search(text)
    if sm:
        sid = int(sm.group(1), 16)
    if sid is None:
        sid = 0x81
    return decode_payload(sid, raw)


def iter_inputs(args: argparse.Namespace) -> Iterable[str]:
    if args.log:
        with open(args.log, "r", errors="replace") as fh:
            for line in fh:
                if ("payload=" in line or "pb=" in line) and SID_RE.search(line):
                    yield line.rstrip("\n")
    for h in args.hex:
        yield h


# ---------------------------------------------------------------------------
# Self-test: the one frame we have actually seen on the air
# ---------------------------------------------------------------------------

def selftest() -> int:
    # G2_PROTOCOL.md records the shape as `08 01 10 <varint> 1A 06 08 0C 10 01`:
    # the inner length says 6 but only 4 inner bytes follow. That matches the
    # firmware's frame ring, which keeps only the first 12 payload bytes
    # (G2_Glasses.cpp head[12]); with a 1-2 byte magic the last two inner bytes
    # fell off the end, and they may be exactly lidStatus / inCaseStatus. The
    # self-test uses the consistent 4-byte form; phase 0 widens the dump.
    observed = "08 01 10 2A 1A 04 08 0C 10 01"
    d = decode_payload(0x81, hex_to_bytes(observed))
    assert d.summary["commandId"] == "1(CASE_INFO)", d
    assert d.summary["magicRandom"] == 0x2A, d
    assert d.summary["caseInfo.soc"] == 12, d
    assert d.summary["caseInfo.chargeStatus"] == 1, d
    assert d.summary["caseInfo.lidStatus"] == "absent(=0?)", d
    assert d.summary["caseInfo.inCaseStatus"] == "absent(=0?)", d
    assert not d.unknown, d

    # Settings async push: silent mode ON (captured 2026-04-26 per G2_PROTOCOL.md)
    d2 = decode_payload(0x09, hex_to_bytes("08 03 10 18 2A 02 10 01"))
    assert d2.summary["commandId"] == "3(DeviceSendToAPP)", d2
    assert d2.summary["push.silentModeSwitch"] == 1, d2

    # BASIC reply: the glasses echo the request in outer f4 with battery 70
    # and chargingStatus 1 (field numbers 12 and 13).
    d3 = decode_payload(0x09, hex_to_bytes("08 04 10 05 22 04 60 46 68 01"))
    assert d3.summary["echo.battery"] == 70, d3
    assert d3.summary["echo.chargingStatus"] == 1, d3
    # Outer f6 is the 2-field ack, not a settings mirror.
    d5 = decode_payload(0x09, hex_to_bytes("08 04 10 05 32 04 08 07 10 00"))
    assert d5.summary["ack.packageId"] == 7 and d5.summary["ack.receiveFlag"] == 0, d5

    # Unknown inner field is surfaced, not dropped.
    d4 = decode_payload(0x09, hex_to_bytes("08 03 10 18 2A 02 38 01"))
    assert d4.unknown == ["push.f7=1"], d4

    # Envelope round trip; CRC covers the payload only.
    pb = hex_to_bytes(observed)
    hdr = bytes([0xAA, 0x12, 0x5C, len(pb) + 2, 1, 1, 0x81, 0x00])
    crc = crc16_ccitt_false(pb)
    env = parse_envelope(hdr + pb + crc.to_bytes(2, "little"))
    assert env.sid == 0x81 and env.crc_ok is True and env.payload == pb, env
    print("selftest OK")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hex", nargs="*", help="payload or envelope hex strings")
    ap.add_argument("--sid", type=lambda s: int(s, 0), default=None, help="sid for bare payloads (default 0x81)")
    ap.add_argument("--log", help="serial log to scan for payload dumps")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    n = 0
    for text in iter_inputs(args):
        try:
            d = decode_text(text, args.sid)
        except ValueError as e:
            print(f"!! {e}: {text.strip()[:80]}")
            continue
        if d is None:
            continue
        n += 1
        print(d.render())
    if n == 0:
        print("nothing to decode (pass hex, or --log with payload= lines)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
