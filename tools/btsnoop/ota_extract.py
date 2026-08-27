#!/usr/bin/env python3
"""
Passive offline extractor for Even G2 OTA / EFS and R1 frames from Android
btsnoop HCI logs.

SAFETY: read-only on the log. Never transmits, MitMs, or ACKs. Run only after
the Even app has finished a successful update, with Hardware One disconnected
during that update. See docs/OTA_PASSIVE_CAPTURE.md.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Tuple

# ── G2 SIDs of interest ──────────────────────────────────────────────────────
SID_OTA_CMD = 0xC0
SID_OTA_RAW = 0xC1
SID_OTA_EXPORT_CMD = 0xC2
SID_OTA_EXPORT_RAW = 0xC3
SID_EFS_CMD = 0xC4
SID_EFS_RAW = 0xC5
SID_EFS_EXPORT_CMD = 0xC6
SID_EFS_EXPORT_RAW = 0xC7

OTA_EFS_SIDS = frozenset(
    {
        SID_OTA_CMD,
        SID_OTA_RAW,
        SID_OTA_EXPORT_CMD,
        SID_OTA_EXPORT_RAW,
        SID_EFS_CMD,
        SID_EFS_RAW,
        SID_EFS_EXPORT_CMD,
        SID_EFS_EXPORT_RAW,
    }
)

SID_NAME = {
    SID_OTA_CMD: "OTA_CMD",
    SID_OTA_RAW: "OTA_RAW",
    SID_OTA_EXPORT_CMD: "OTA_EXPORT_CMD",
    SID_OTA_EXPORT_RAW: "OTA_EXPORT_RAW",
    SID_EFS_CMD: "EFS_CMD",
    SID_EFS_RAW: "EFS_RAW",
    SID_EFS_EXPORT_CMD: "EFS_EXPORT_CMD",
    SID_EFS_EXPORT_RAW: "EFS_EXPORT_RAW",
}

OTA_CID_NAME = {
    0: "BEGIN",           # g2flash: op 0x00 begin
    1: "FILE_CHECK",      # op 0x01 + 128B EVENOTA subheader
    2: "DATA_MARKER",     # op 0x02 before each 4KB block
    3: "END",             # op 0x03 RESULT_CHECK
    4: "NOTIFY",
}

# Known G2 GATT UUIDs (g2flash / FINDINGS). HCI often only has handles;
# we map UUID↔handle when discovery appears in the log.
UUID_DATA_SVC = "00002760-08c2-11e1-9073-0e8ac72e1001"
UUID_DATA_WRITE = "00002760-08c2-11e1-9073-0e8ac72e0001"
UUID_DATA_NOTIFY = "00002760-08c2-11e1-9073-0e8ac72e0002"
UUID_CTRL_SVC = "00002760-08c2-11e1-9073-0e8ac72e5450"
UUID_CTRL_WRITE = "00002760-08c2-11e1-9073-0e8ac72e5401"
UUID_CTRL_NOTIFY = "00002760-08c2-11e1-9073-0e8ac72e5402"

KNOWN_UUID_BYTES = {
    bytes.fromhex("0000276008c211e190730e8ac72e0001"): "DATA_WRITE_e0001",
    bytes.fromhex("0000276008c211e190730e8ac72e0002"): "DATA_NOTIFY_e0002",
    bytes.fromhex("0000276008c211e190730e8ac72e1001"): "DATA_SVC_e1001",
    bytes.fromhex("0000276008c211e190730e8ac72e5401"): "CTRL_WRITE_e5401",
    bytes.fromhex("0000276008c211e190730e8ac72e5402"): "CTRL_NOTIFY_e5402",
    bytes.fromhex("0000276008c211e190730e8ac72e5450"): "CTRL_SVC_e5450",
}

EFS_SUB_NAME = {
    0: "SEND_START",
    1: "DATA_ANNOUNCE",
    2: "RESULT_CHECK",
}

R1_SUB_NAME = {
    0x01: "deviceStatus",
    0x02: "deviceInfo",
    0x03: "wearStatus",
    0x04: "userInfo",
    0x05: "systemTime",
    0x06: "touchStatus",
    0x07: "touchSwitch",
    0x08: "pairAuth",
    0x09: "otaStart",
    0x0A: "advStart",
    0x0B: "getAlgoKeyStatus",
    0x0C: "setAlgoKey",
    0x0E: "healthSettings",
    0x0F: "systemSettings",
    0x10: "deviceSN",
    0x11: "nvRecover",
    0x12: "powerControl",
    0x7E: "packetAck",
    0x7F: "heartbeat",
}


# ── CRC helpers (match firmware) ─────────────────────────────────────────────

def g2_crc16_ccitt_false(data: bytes) -> int:
    """CRC-16/CCITT-FALSE — same as System_G2_Protocol.cpp g2CrcCcittFalse."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


def r1_crc32_castagnoli(data: bytes) -> int:
    """CRC-32C Castagnoli, init=0, no reflect, no xorout (r1Crc32)."""
    poly = 0x1EDC6F41
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ poly) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc


def r1_crc16(model: bytes) -> int:
    """R1 model CRC16 over bytes [0..3]+[5..9]+[12..] — CCITT-ish init 0xFFFF."""
    # Build covered slice
    parts = bytearray()
    if len(model) < 12:
        return 0
    parts += model[0:4]
    parts += model[5:10]
    if len(model) > 12:
        parts += model[12:]
    crc = 0xFFFF
    for b in parts:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return crc


# ── btsnoop / ATT ────────────────────────────────────────────────────────────

@dataclass
class AttValue:
    rec: int
    ts: int
    direction: str  # "TX" (host→controller sent) / "RX" based on flags+op
    op: int
    handle: int
    value: bytes


def iter_btsnoop_records(data: bytes) -> Iterable[Tuple[int, int, int, bytes]]:
    """Yield (rec_index, timestamp_us, flags, packet_including_hci_type)."""
    if len(data) < 16 or data[:8] != b"btsnoop\x00":
        raise ValueError("not a btsnoop file (missing magic)")
    version, datalink = struct.unpack_from(">II", data, 8)
    # 1001 = H1, 1002 = H4 (HCI UART)
    off = 16
    idx = 0
    while off + 24 <= len(data):
        orig_len, inc_len, flags, _drops, ts = struct.unpack_from(">IIIIQ", data, off)
        off += 24
        pkt = data[off : off + inc_len]
        off += inc_len
        yield idx, ts, flags, pkt
        idx += 1
    _ = version, datalink


def extract_att_values(data: bytes) -> List[AttValue]:
    """Pull ATT Write/Notify/Indicate values from H4 ACL records."""
    out: List[AttValue] = []
    for rec, ts, flags, pkt in iter_btsnoop_records(data):
        if not pkt:
            continue
        # H4: type byte 2 = ACL
        if pkt[0] != 0x02:
            continue
        acl = pkt[1:]
        if len(acl) < 8:
            continue
        acl_len = struct.unpack_from("<H", acl, 2)[0]
        payload = acl[4 : 4 + acl_len]
        if len(payload) < 8:
            continue
        l2_len, cid = struct.unpack_from("<HH", payload, 0)
        # ATT CID = 0x0004
        if cid != 0x0004:
            continue
        att = payload[4 : 4 + l2_len]
        if len(att) < 3:
            continue
        op = att[0]
        # Write Request 0x12, Write Command 0x52, Handle Value NTF 0x1B, IND 0x1D
        if op not in (0x12, 0x52, 0x1B, 0x1D):
            continue
        handle = struct.unpack_from("<H", att, 1)[0]
        value = att[3:]
        # flags bit0: 0 = sent by host, 1 = received by host (typical btsnoop)
        host_sent = (flags & 1) == 0
        if op in (0x12, 0x52):
            direction = "TX" if host_sent else "RX"
        else:
            # notifications come from peripheral → host receive
            direction = "RX" if not host_sent else "TX"
        out.append(
            AttValue(
                rec=rec,
                ts=ts,
                direction=direction,
                op=op,
                handle=handle,
                value=value,
            )
        )
    return out


# ── G2 envelope reassembly ───────────────────────────────────────────────────

@dataclass
class G2Message:
    rec: int
    last_rec: int
    ts: int
    direction: str  # TX=AA21 host→device, RX=AA12 device→host
    seq: int
    sid: int
    flag: int
    pb: bytes
    crc_ok: Optional[bool]
    frags: int
    handle: int


def parse_g2_messages(att_values: List[AttValue]) -> List[G2Message]:
    pending: Dict[Tuple[str, int, int], List[Tuple[int, bytes, AttValue, int, bool]]] = defaultdict(
        list
    )
    msgs: List[G2Message] = []

    for av in att_values:
        v = av.value
        if len(v) < 10:
            continue
        if v[0] != 0xAA or v[1] not in (0x21, 0x12):
            continue
        direction = "TX" if v[1] == 0x21 else "RX"
        seq, declared, tot, idx, sid, flag = v[2], v[3], v[4], v[5], v[6], v[7]
        if tot == 0 or idx == 0 or idx > tot:
            continue
        is_last = idx == tot
        if is_last:
            if declared < 2 or 8 + declared > len(v):
                pb_part = v[8:-2] if len(v) > 10 else b""
            else:
                pb_part = v[8 : 8 + declared - 2]
        else:
            if 8 + declared > len(v):
                pb_part = v[8:]
            else:
                pb_part = v[8 : 8 + declared]

        key = (direction, seq, sid)
        pending[key].append((idx, pb_part, av, declared, is_last))

        if is_last:
            parts = sorted(pending.pop(key), key=lambda x: x[0])
            pb = b"".join(p[1] for p in parts)
            last_av = parts[-1][2]
            last_val = last_av.value
            dec = parts[-1][3]
            if 8 + dec <= len(last_val) and dec >= 2:
                rcv = last_val[8 + dec - 2] | (last_val[8 + dec - 1] << 8)
            elif len(last_val) >= 2:
                rcv = last_val[-2] | (last_val[-1] << 8)
            else:
                rcv = -1
            calc = g2_crc16_ccitt_false(pb) if pb is not None else -1
            crc_ok = (rcv == calc) if rcv >= 0 else None
            msgs.append(
                G2Message(
                    rec=parts[0][2].rec,
                    last_rec=last_av.rec,
                    ts=parts[0][2].ts,
                    direction=direction,
                    seq=seq,
                    sid=sid,
                    flag=flag,
                    pb=pb,
                    crc_ok=crc_ok,
                    frags=len(parts),
                    handle=parts[0][2].handle,
                )
            )
    return msgs


# ── Heuristic cmd decode ─────────────────────────────────────────────────────

def guess_ota_cid(pb: bytes) -> Optional[int]:
    """Packed first-byte op (g2flash DATA channel) or protobuf field1."""
    if not pb:
        return None
    if pb[0] <= 4:
        return pb[0]
    if pb[0] == 0x08 and len(pb) >= 2:
        return pb[1] & 0x7F
    return None


def parse_file_check_subheader(pb: bytes) -> Optional[Dict[str, Any]]:
    """C0 op=0x01 FILE_CHECK: [0x01][128B EVENOTA component subheader]."""
    if len(pb) < 1 + 128 or pb[0] != 0x01:
        return None
    sub = pb[1:129]
    ps = struct.unpack_from("<I", sub, 8)[0]
    crc = struct.unpack_from("<I", sub, 12)[0]
    fn = sub[48:128].split(b"\0", 1)[0].decode("latin1", errors="replace")
    return {
        "op": 1,
        "payload_size": ps,
        "crc32c": f"0x{crc:08x}",
        "filename": fn,
        "subheader_hex": sub.hex(),
    }


def parse_efs_start(pb: bytes) -> Optional[Dict[str, Any]]:
    """77-byte EFS SEND_START layout from G2_Glasses native notify path."""
    if len(pb) < 13:
        return None
    if pb[0] != 0x00:
        return None
    # Distinguish from OTA BEGIN (also 0x00 but typically short)
    if len(pb) < 13:
        return None
    file_type = struct.unpack_from("<I", pb, 1)[0]
    data_len = struct.unpack_from("<I", pb, 5)[0]
    file_crc = struct.unpack_from("<I", pb, 9)[0]
    path = b""
    if len(pb) >= 77:
        path = pb[13:77].split(b"\x00", 1)[0]
    elif len(pb) > 13:
        path = pb[13:].split(b"\x00", 1)[0]
    try:
        path_s = path.decode("utf-8", errors="replace")
    except Exception:
        path_s = path.hex()
    return {
        "subCmd": 0,
        "fileType": file_type,
        "dataLength": data_len,
        "fileDataCrc32": f"0x{file_crc:08x}",
        "path": path_s,
        "packedLen": len(pb),
    }


def discover_uuid_handles(raw: bytes) -> Dict[int, str]:
    """Best-effort: find known 128-bit UUIDs in the HCI blob and nearby handles.

    Looks for ATT Find Information (0x05) / Read By Type (0x09) style pairs and
    also a simple 'UUID bytes near a LE u16 handle' heuristic.
    """
    mapping: Dict[int, str] = {}
    for ubytes, name in KNOWN_UUID_BYTES.items():
        # UUID may appear LE-reversed in ATT
        variants = (ubytes, ubytes[::-1])
        for needle in variants:
            start = 0
            while True:
                j = raw.find(needle, start)
                if j < 0:
                    break
                # handle often immediately before UUID in Find Info format
                if j >= 2:
                    h = raw[j - 2] | (raw[j - 1] << 8)
                    if 0x0001 <= h <= 0xFFFF:
                        mapping.setdefault(h, name)
                start = j + 1
    return mapping


def crc32c_msb(buf: bytes) -> int:
    table: List[int] = []
    for b in range(256):
        c = b << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x1EDC6F41) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        table.append(c)
    crc = 0
    for byte in buf:
        crc = ((crc << 8) & 0xFFFFFFFF) ^ table[((crc >> 24) ^ byte) & 0xFF]
    return crc


# ── R1 observe ───────────────────────────────────────────────────────────────

@dataclass
class R1Frame:
    rec: int
    ts: int
    direction: str
    handle: int
    transfer_type: int
    model: bytes
    module: int
    cmd: int
    sub_cmd: int
    status: int
    payload: bytes
    crc32_ok: Optional[bool]
    crc16_ok: Optional[bool]
    file_like: bool


def try_parse_r1(av: AttValue) -> Optional[R1Frame]:
    v = av.value
    if len(v) < 5 + 12:
        return None
    # Skip G2 envelopes
    if v[0] == 0xAA and len(v) > 1 and v[1] in (0x21, 0x12):
        return None
    transfer_type = v[0]
    # Normal cmd frames use 0x00; file-transfer uses nonzero — still observe
    wire_crc = struct.unpack_from("<I", v, 1)[0]
    model = v[5:]
    if len(model) < 12:
        return None
    if model[0] != 0x64:  # version
        return None
    module = model[1]
    if module not in (0x01, 0x02, 0x03, 0x7F):
        return None
    if model[2] != 0x64:  # moduleVersion
        return None
    model_len = model[8] | (model[9] << 8)
    if model_len < 12 or model_len > len(model) + 64:
        # allow slight slack; still require header consistency
        if model_len < 12:
            return None
    status = model[5]
    cmd = model[6]
    sub = model[7]
    payload = model[12:model_len] if model_len <= len(model) else model[12:]

    calc32 = r1_crc32_castagnoli(model[:model_len] if model_len <= len(model) else model)
    # Some stacks CRC the full model buffer as sent
    crc32_ok = wire_crc == calc32 or wire_crc == r1_crc32_castagnoli(model)

    wire16 = model[10] | (model[11] << 8)
    crc16_ok = wire16 == r1_crc16(model[:model_len] if model_len <= len(model) else model)

    file_like = transfer_type != 0x00 or sub == 0x09  # otaStart
    return R1Frame(
        rec=av.rec,
        ts=av.ts,
        direction=av.direction,
        handle=av.handle,
        transfer_type=transfer_type,
        model=model,
        module=module,
        cmd=cmd,
        sub_cmd=sub,
        status=status,
        payload=payload,
        crc32_ok=crc32_ok,
        crc16_ok=crc16_ok,
        file_like=file_like,
    )


# ── Extract orchestration ────────────────────────────────────────────────────

@dataclass
class ExtractStats:
    att_values: int = 0
    g2_msgs: int = 0
    g2_ota_efs: int = 0
    g2_crc_fail: int = 0
    r1_frames: int = 0
    r1_ota_or_file: int = 0
    ota_components: int = 0
    data_handles: int = 0
    notes: List[str] = field(default_factory=list)


def extract(log_path: Path, out_dir: Path) -> ExtractStats:
    data = log_path.read_bytes()
    out_dir.mkdir(parents=True, exist_ok=True)
    stats = ExtractStats()

    handle_names = discover_uuid_handles(data)
    if handle_names:
        (out_dir / "gatt_handles.json").write_text(
            json.dumps({f"0x{h:04x}": n for h, n in sorted(handle_names.items())}, indent=2)
            + "\n",
            encoding="utf-8",
        )

    att = extract_att_values(data)
    stats.att_values = len(att)

    g2_msgs = parse_g2_messages(att)
    stats.g2_msgs = len(g2_msgs)

    timeline_lines: List[str] = []
    cmds_jsonl: List[str] = []
    r1_jsonl: List[str] = []

    # Raw concatenations keyed by stream
    ota_raw_parts: List[bytes] = []
    ota_export_parts: List[bytes] = []
    efs_raw_parts: List[bytes] = []
    efs_export_parts: List[bytes] = []
    efs_sessions: List[Dict[str, Any]] = []
    current_efs: Optional[Dict[str, Any]] = None

    # Per-handle dumps (e1001 DATA channel traffic shows up as AA21 on its write handle)
    handle_c0: Dict[int, List[bytes]] = defaultdict(list)
    handle_c1: Dict[int, List[bytes]] = defaultdict(list)
    handle_all_ota: Dict[int, List[Dict[str, Any]]] = defaultdict(list)

    # Component reconstruction (g2flash: FILE_CHECK → 4KB C1 blocks → END)
    components: List[Dict[str, Any]] = []
    cur_comp: Optional[Dict[str, Any]] = None

    ota_part_idx = 0
    ota_raw_open: List[bytes] = []

    def flush_ota_part():
        nonlocal ota_part_idx, ota_raw_open
        if not ota_raw_open:
            return
        blob = b"".join(ota_raw_open)
        ota_raw_parts.append(blob)
        (out_dir / f"g2_ota_raw_part{ota_part_idx:02d}.bin").write_bytes(blob)
        ota_part_idx += 1
        ota_raw_open = []

    def finish_component(reason: str):
        nonlocal cur_comp
        if cur_comp is None:
            return
        blob = b"".join(cur_comp["blocks"])
        cur_comp["assembled_bytes"] = len(blob)
        cur_comp["end_reason"] = reason
        expected = cur_comp.get("payload_size") or 0
        if expected and blob:
            # truncate/pad note only; keep full assembled
            calc = crc32c_msb(blob[:expected]) if len(blob) >= expected else crc32c_msb(blob)
            want = int(cur_comp["crc32c"], 16) if isinstance(cur_comp["crc32c"], str) else cur_comp["crc32c"]
            cur_comp["crc_calc"] = f"0x{calc:08x}"
            cur_comp["crc_ok"] = calc == want and len(blob) >= expected
        idx = len(components)
        fn = cur_comp.get("filename") or f"component_{idx}"
        safe = "".join(c if c.isalnum() or c in "._-+" else "_" for c in fn.replace("/", "_"))
        out_name = f"g2_ota_component_{idx:02d}_{safe}.bin"
        (out_dir / out_name).write_bytes(blob)
        cur_comp["out_file"] = out_name
        # drop block bytes from json
        meta = {k: v for k, v in cur_comp.items() if k != "blocks"}
        components.append(meta)
        cur_comp = None

    interest = [m for m in g2_msgs if m.sid in OTA_EFS_SIDS]
    stats.g2_ota_efs = len(interest)
    stats.g2_crc_fail = sum(1 for m in interest if m.crc_ok is False)

    for m in interest:
        sid_name = SID_NAME.get(m.sid, f"SID_0x{m.sid:02X}")
        crc_s = {True: "ok", False: "FAIL", None: "?"}.get(m.crc_ok, "?")
        hname = handle_names.get(m.handle, "")
        note = ""
        meta: Dict[str, Any] = {
            "rec": m.rec,
            "last_rec": m.last_rec,
            "ts": m.ts,
            "dir": m.direction,
            "seq": m.seq,
            "sid": m.sid,
            "sid_name": sid_name,
            "flag": m.flag,
            "frags": m.frags,
            "crc_ok": m.crc_ok,
            "pb_len": len(m.pb),
            "pb_hex": m.pb.hex(),
            "handle": m.handle,
            "handle_name": hname or None,
        }

        if m.sid in (SID_OTA_CMD, SID_OTA_EXPORT_CMD):
            cid = guess_ota_cid(m.pb)
            meta["cid"] = cid
            meta["cid_name"] = OTA_CID_NAME.get(cid, "?") if cid is not None else None
            note = f"op={meta['cid_name']}"
            if m.sid == SID_OTA_CMD:
                handle_c0[m.handle].append(m.pb)
                handle_all_ota[m.handle].append(meta)
                fc = parse_file_check_subheader(m.pb)
                if fc:
                    if cur_comp is not None:
                        finish_component("new_file_check")
                    cur_comp = {
                        **fc,
                        "handle": m.handle,
                        "handle_name": hname or None,
                        "rec": m.rec,
                        "blocks": [],
                    }
                    note = (
                        f"FILE_CHECK {fc['filename']!r} "
                        f"ps={fc['payload_size']} crc={fc['crc32c']}"
                    )
                    meta["file_check"] = fc
                elif cid == 0:
                    note = "BEGIN"
                    if ota_raw_open:
                        flush_ota_part()
                elif cid == 2:
                    note = "DATA_MARKER"
                elif cid == 3:
                    note = "END"
                    finish_component("end")
                    if ota_raw_open:
                        flush_ota_part()
            efs_like = parse_efs_start(m.pb) if len(m.pb) >= 77 else None
            if efs_like:
                meta["efs_like_start"] = efs_like

        elif m.sid == SID_OTA_RAW:
            handle_c1[m.handle].append(m.pb)
            handle_all_ota[m.handle].append(meta)
            if m.direction == "TX":
                ota_raw_open.append(m.pb)
                if cur_comp is not None:
                    cur_comp["blocks"].append(m.pb)
            note = f"raw_chunk={len(m.pb)}"

        elif m.sid == SID_OTA_EXPORT_RAW:
            ota_export_parts.append(m.pb)
            note = f"export_chunk={len(m.pb)}"

        elif m.sid in (SID_EFS_CMD, SID_EFS_EXPORT_CMD):
            if len(m.pb) == 1 and m.pb[0] in EFS_SUB_NAME:
                meta["efs_sub"] = m.pb[0]
                meta["efs_sub_name"] = EFS_SUB_NAME[m.pb[0]]
                note = meta["efs_sub_name"]
                if m.pb[0] == 0x01 and current_efs is not None:
                    current_efs["announced"] = True
                if m.pb[0] == 0x02 and current_efs is not None:
                    current_efs["result_check"] = True
                    efs_sessions.append(current_efs)
                    current_efs = None
            else:
                start = parse_efs_start(m.pb)
                if start:
                    meta["efs_start"] = start
                    note = f"START type={start['fileType']} len={start['dataLength']} path={start['path']!r}"
                    current_efs = {
                        "start": start,
                        "raw_chunks": 0,
                        "raw_bytes": 0,
                        "dir": m.direction,
                        "rec": m.rec,
                    }
                else:
                    cid = guess_ota_cid(m.pb)
                    meta["cid_guess"] = cid
                    note = f"cmd_pb_len={len(m.pb)}"

        elif m.sid == SID_EFS_RAW:
            efs_raw_parts.append(m.pb)
            if current_efs is not None:
                current_efs["raw_chunks"] += 1
                current_efs["raw_bytes"] += len(m.pb)
            note = f"efs_raw={len(m.pb)}"

        elif m.sid == SID_EFS_EXPORT_RAW:
            efs_export_parts.append(m.pb)
            note = f"efs_export_raw={len(m.pb)}"

        htag = f" handle=0x{m.handle:04x}"
        if hname:
            htag += f"({hname})"
        line = (
            f"rec={m.rec:6d} {m.direction:2s} seq={m.seq:3d} "
            f"sid=0x{m.sid:02X}({sid_name:14s}) frags={m.frags} "
            f"pb={len(m.pb):6d} crc={crc_s:4s}{htag} {note}"
        )
        timeline_lines.append(line)
        cmds_jsonl.append(json.dumps(meta, separators=(",", ":")))

    flush_ota_part()
    finish_component("eof")
    if current_efs is not None:
        efs_sessions.append(current_efs)

    stats.ota_components = len(components)
    stats.data_handles = len(handle_c1) or len(handle_c0)

    # Combined OTA raw
    if ota_raw_parts:
        (out_dir / "g2_ota_raw.bin").write_bytes(b"".join(ota_raw_parts))
    if ota_export_parts:
        (out_dir / "g2_ota_export_raw.bin").write_bytes(b"".join(ota_export_parts))
    if efs_raw_parts:
        (out_dir / "efs_raw.bin").write_bytes(b"".join(efs_raw_parts))
    if efs_export_parts:
        (out_dir / "efs_export_raw.bin").write_bytes(b"".join(efs_export_parts))

    # Per-handle DATA-channel dumps (e1001 write path)
    handles_dir = out_dir / "by_handle"
    if handle_c0 or handle_c1:
        handles_dir.mkdir(exist_ok=True)
        for h, chunks in sorted(handle_c1.items()):
            tag = handle_names.get(h, "unknown")
            (handles_dir / f"0x{h:04x}_{tag}_c1_raw.bin").write_bytes(b"".join(chunks))
        for h, chunks in sorted(handle_c0.items()):
            tag = handle_names.get(h, "unknown")
            (handles_dir / f"0x{h:04x}_{tag}_c0_cmds.bin").write_bytes(b"".join(chunks))
        (handles_dir / "index.json").write_text(
            json.dumps(
                {
                    f"0x{h:04x}": {
                        "name": handle_names.get(h),
                        "c0_msgs": len(handle_c0.get(h, [])),
                        "c1_chunks": len(handle_c1.get(h, [])),
                        "c1_bytes": sum(len(x) for x in handle_c1.get(h, [])),
                    }
                    for h in sorted(set(handle_c0) | set(handle_c1))
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

    if components:
        (out_dir / "g2_ota_components.json").write_text(
            json.dumps(components, indent=2) + "\n", encoding="utf-8"
        )

    for i, sess in enumerate(efs_sessions):
        (out_dir / f"efs_session_{i:02d}.json").write_text(
            json.dumps(sess, indent=2) + "\n", encoding="utf-8"
        )

    # R1 frames — observe only
    r1_file_blobs: List[bytes] = []
    for av in att:
        rf = try_parse_r1(av)
        if rf is None:
            continue
        # Prefer frames that look real (crc hints) or are interesting
        if rf.crc16_ok is False and rf.crc32_ok is False and not rf.file_like:
            continue
        stats.r1_frames += 1
        if rf.file_like:
            stats.r1_ota_or_file += 1
            if rf.payload:
                r1_file_blobs.append(rf.payload)

        sub_name = R1_SUB_NAME.get(rf.sub_cmd, f"0x{rf.sub_cmd:02x}")
        warn = ""
        if rf.sub_cmd == 0x09:
            warn = "  # OBSERVE ONLY — never send otaStart from our stack"
        line = (
            f"rec={rf.rec:6d} {rf.direction:2s} R1 handle=0x{rf.handle:04x} "
            f"xfer=0x{rf.transfer_type:02x} mod={rf.module} cmd={rf.cmd} "
            f"sub={sub_name} status=0x{rf.status:02x} pay={len(rf.payload)} "
            f"crc32={rf.crc32_ok} crc16={rf.crc16_ok}{warn}"
        )
        timeline_lines.append(line)
        r1_jsonl.append(
            json.dumps(
                {
                    "rec": rf.rec,
                    "ts": rf.ts,
                    "dir": rf.direction,
                    "handle": rf.handle,
                    "transferType": rf.transfer_type,
                    "module": rf.module,
                    "cmd": rf.cmd,
                    "subCmd": rf.sub_cmd,
                    "subCmdName": sub_name,
                    "status": rf.status,
                    "payload_len": len(rf.payload),
                    "payload_hex": rf.payload.hex(),
                    "crc32_ok": rf.crc32_ok,
                    "crc16_ok": rf.crc16_ok,
                    "file_like": rf.file_like,
                },
                separators=(",", ":"),
            )
        )

    if r1_file_blobs:
        (out_dir / "r1_file_payloads.bin").write_bytes(b"".join(r1_file_blobs))

    # Sort timeline by rec (G2 interest + R1 mixed — re-sort)
    def rec_key(line: str) -> int:
        try:
            return int(line.split("rec=", 1)[1].split()[0])
        except Exception:
            return 0

    timeline_lines.sort(key=rec_key)

    (out_dir / "timeline.txt").write_text(
        "\n".join(timeline_lines) + ("\n" if timeline_lines else ""),
        encoding="utf-8",
    )
    (out_dir / "g2_ota_cmds.jsonl").write_text(
        "\n".join(cmds_jsonl) + ("\n" if cmds_jsonl else ""),
        encoding="utf-8",
    )
    (out_dir / "r1_frames.jsonl").write_text(
        "\n".join(r1_jsonl) + ("\n" if r1_jsonl else ""),
        encoding="utf-8",
    )

    # README
    readme = f"""OTA / EFS / R1 passive extract
================================
Source log : {log_path}
Output dir : {out_dir}

THIS TOOL IS READ-ONLY. It never transmits. Capture must be taken while the
Even phone app alone owns the BLE link (Hardware One disconnected).

See docs/OTA_RESEARCH_FINDINGS_2026-07-31.md and tools/btsnoop/README.md.
G2 flash: AA21 SID 0xC0/0xC1 on DATA …e0001/…e0002.
R1 flash: separate tool r1_dfu_extract.py (Nordic Secure DFU after otaStart).
Prefer btsnoop_hci.combined.log from pull_hci.sh for long G2 flashes.

Stats
-----
ATT values scanned     : {stats.att_values}
G2 messages reassembled: {stats.g2_msgs}
G2 OTA/EFS messages    : {stats.g2_ota_efs}
G2 OTA/EFS CRC fails   : {stats.g2_crc_fail}
OTA components rebuilt : {stats.ota_components}
OTA DATA handles       : {stats.data_handles}
R1 frames observed     : {stats.r1_frames}
R1 ota/file-like       : {stats.r1_ota_or_file}
GATT UUID hits         : {len(handle_names)}

Outputs
-------
timeline.txt              Chronological G2 OTA/EFS + interesting R1
g2_ota_cmds.jsonl         Per-envelope metadata + pb_hex
g2_ota_raw.bin            All host→device C1 chunks concatenated
g2_ota_raw_partNN.bin     Split on BEGIN/FILE_CHECK/END
g2_ota_component_NN_*.bin Rebuilt from FILE_CHECK + C1 blocks (g2flash layout)
g2_ota_components.json    Component metadata + CRC32C check
by_handle/                Per-ATT-handle C0/C1 dumps (e1001 path)
gatt_handles.json         UUID↔handle guesses from discovery bytes
efs_*.bin / efs_session_* EFS file-service reconstruct
r1_frames.jsonl           Observe-only (never send otaStart)
                          For R1 firmware bytes use r1_dfu_extract.py

G2 DATA channel ops (SID 0xC0)
------------------------------
0x00 BEGIN   0x01 FILE_CHECK(+128B subheader)   0x02 DATA_MARKER   0x03 END
SID 0xC1 carries 4096-byte blocks (fragmented).
"""
    if stats.r1_ota_or_file and stats.g2_ota_efs == 0:
        stats.notes.append(
            "R1 otaStart/file-like frames seen. Reconstruct firmware with "
            "tools/btsnoop/r1_dfu_extract.py (this tool only observes R1)."
        )
        readme += "\nNOTE: " + stats.notes[-1] + "\n"
    if stats.g2_ota_efs == 0 and stats.r1_ota_or_file == 0:
        stats.notes.append(
            "No OTA/EFS SID traffic and no R1 ota/file-like frames found. "
            "Was HCI snoop enabled for the whole update? Try "
            "btsnoop_hci.combined.log if the flash spanned a snoop rotation."
        )
        readme += "\nWARNING: " + stats.notes[-1] + "\n"

    (out_dir / "README_EXTRACT.txt").write_text(readme, encoding="utf-8")
    return stats


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(
        description="Passive offline G2 OTA/EFS + R1 extract from btsnoop (read-only)."
    )
    ap.add_argument("log", type=Path, help="path to btsnoop_hci.log")
    ap.add_argument(
        "-o",
        "--out",
        type=Path,
        default=None,
        help="output directory (default: <log_dir>/out)",
    )
    args = ap.parse_args(argv)
    log_path: Path = args.log
    if not log_path.is_file():
        print(f"error: log not found: {log_path}", file=sys.stderr)
        return 1
    out_dir = args.out if args.out else log_path.parent / "out"

    print(f"Reading {log_path} ({log_path.stat().st_size} bytes)")
    print(f"Writing {out_dir}")
    try:
        stats = extract(log_path, out_dir)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    print(
        f"Done: att={stats.att_values} g2={stats.g2_msgs} "
        f"ota_efs={stats.g2_ota_efs} comps={stats.ota_components} "
        f"handles={stats.data_handles} crc_fail={stats.g2_crc_fail} "
        f"r1={stats.r1_frames} r1_fileish={stats.r1_ota_or_file}"
    )
    for n in stats.notes:
        print(f"NOTE: {n}")
    print(f"See {out_dir / 'README_EXTRACT.txt'} and timeline.txt")
    return 0


if __name__ == "__main__":
    sys.exit(main())
