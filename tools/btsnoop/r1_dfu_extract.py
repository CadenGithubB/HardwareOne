#!/usr/bin/env python3
"""
Passive offline reconstructor for Even R1 Nordic Secure DFU from Android
btsnoop HCI logs.

SAFETY: read-only on the log. Never transmits. R1 otaStart / DFU writes must
never be replayed from our stack. See docs/OTA_PASSIVE_CAPTURE.md.

Wire (validated 2026-07-31 HCI capture):
  1. App sends R1 subCmd otaStart (observe-only) on the normal ring chars.
  2. ACL disconnect, then reconnect into DFU mode.
  3. Nordic Secure DFU session (not SMP/MCUmgr on the wire):
       Control Point  — ATT writes/notifies (SELECT/CREATE/SET_PRN/
                        CALC_CRC/EXECUTE; responses 0x60 …)
       Packet         — ATT write-commands carrying object bytes
  4. Init object (type=1) then data pages (type=2, typically 4096 B).
  5. Integrity:
       - Device CALC_CRC: zlib/IEEE; data CRCs are cumulative over the image
       - InitCommand.hash: SHA256(image) with digest bytes reversed
         (nrfutil little-endian) — see r1_fw_analyze.py

Handles are auto-detected from DFU opcodes; packet handle is scored only
inside CREATE→CRC windows so concurrent G2 OTA traffic cannot win.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
import zlib
import zipfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ATT
ATT_WRITE_REQ = 0x12
ATT_WRITE_CMD = 0x52
ATT_HANDLE_NTF = 0x1B
ATT_HANDLE_IND = 0x1D
ATT_VALUE_OPS = frozenset({ATT_WRITE_REQ, ATT_WRITE_CMD, ATT_HANDLE_NTF, ATT_HANDLE_IND})

# Nordic Secure DFU control opcodes (host→device)
DFU_CREATE = 0x01
DFU_SET_PRN = 0x02
DFU_CALC_CRC = 0x03
DFU_EXECUTE = 0x04
DFU_SELECT = 0x06
DFU_RESPONSE = 0x60

OBJ_COMMAND = 0x01  # init packet
OBJ_DATA = 0x02  # firmware image pages

OP_NAME = {
    DFU_CREATE: "CREATE",
    DFU_SET_PRN: "SET_PRN",
    DFU_CALC_CRC: "CALC_CRC",
    DFU_EXECUTE: "EXECUTE",
    DFU_SELECT: "SELECT",
    DFU_RESPONSE: "RESPONSE",
}


@dataclass
class AttValue:
    rec: int
    ts: int
    tx: bool  # host→controller
    handle: int
    att_op: int
    payload: bytes


@dataclass
class DfuObject:
    obj_type: int
    declared_size: int
    create_rec: int
    data: bytearray = field(default_factory=bytearray)
    device_offset: Optional[int] = None
    device_crc: Optional[int] = None
    local_crc: Optional[int] = None
    crc_ok: Optional[bool] = None


def iter_btsnoop_att(path: Path) -> List[AttValue]:
    raw = path.read_bytes()
    if len(raw) < 16:
        raise ValueError("btsnoop too small")
    # btsnoop identification: "btsnoop\0" + version + datalink
    off = 16
    rec = 0
    out: List[AttValue] = []
    while off + 24 <= len(raw):
        rec += 1
        orig_len, incl_len, flags, _drops, ts = struct.unpack(">IIIIQ", raw[off : off + 24])
        pkt = raw[off + 24 : off + 24 + incl_len]
        off += 24 + incl_len
        if incl_len < 5 or not pkt or pkt[0] != 0x02:  # HCI ACL
            continue
        tx = (flags & 1) == 0
        acl_len = struct.unpack_from("<H", pkt, 3)[0]
        data = pkt[5 : 5 + acl_len]
        if len(data) < 4:
            continue
        l2_len, cid = struct.unpack_from("<HH", data, 0)
        if cid != 0x0004:  # ATT
            continue
        att = data[4 : 4 + l2_len]
        if len(att) < 3 or att[0] not in ATT_VALUE_OPS:
            continue
        handle = struct.unpack_from("<H", att, 1)[0]
        out.append(
            AttValue(
                rec=rec,
                ts=ts,
                tx=tx,
                handle=handle,
                att_op=att[0],
                payload=att[3:],
            )
        )
    return out


def detect_dfu_handles(att: List[AttValue]) -> Tuple[int, int]:
    """Return (control_handle, packet_handle).

    Packet handle must be scored only inside CREATE→CALC_CRC windows so a
    concurrent G2 OTA on …e0001 (huge TX volume) does not win.
    """
    ctrl_score: Dict[int, int] = {}
    for a in att:
        p = a.payload
        if not p:
            continue
        if a.tx and p[0] == DFU_SELECT and len(p) >= 2 and p[1] in (OBJ_COMMAND, OBJ_DATA):
            ctrl_score[a.handle] = ctrl_score.get(a.handle, 0) + 10
        if (not a.tx) and p[0] == DFU_RESPONSE and len(p) >= 3:
            ctrl_score[a.handle] = ctrl_score.get(a.handle, 0) + 3
        if a.tx and p[0] == DFU_CREATE and len(p) >= 6:
            ctrl_score[a.handle] = ctrl_score.get(a.handle, 0) + 5

    if not ctrl_score:
        raise ValueError(
            "no Nordic Secure DFU control traffic found "
            "(expected SELECT/CREATE/0x60 responses)"
        )
    ctrl = max(ctrl_score, key=ctrl_score.get)

    # Bytes written to other handles while a DFU object is open on `ctrl`.
    tx_bytes: Dict[int, int] = {}
    open_obj = False
    for a in att:
        if a.handle == ctrl and a.payload:
            op = a.payload[0]
            if a.tx and op == DFU_CREATE:
                open_obj = True
            elif a.tx and op in (DFU_CALC_CRC, DFU_EXECUTE):
                # still open through CRC; close after execute
                if op == DFU_EXECUTE:
                    open_obj = False
            continue
        if open_obj and a.tx and a.handle != ctrl:
            tx_bytes[a.handle] = tx_bytes.get(a.handle, 0) + len(a.payload)

    if not tx_bytes:
        raise ValueError("no candidate DFU packet handle inside CREATE windows")
    packet = max(tx_bytes, key=tx_bytes.get)
    if tx_bytes[packet] < 64:
        raise ValueError(f"packet handle 0x{packet:04x} only has {tx_bytes[packet]} TX bytes")
    return ctrl, packet


def reconstruct(
    att: List[AttValue],
    ctrl_h: int,
    pkt_h: int,
) -> Tuple[Optional[bytes], bytes, List[DfuObject], List[str], List[Dict[str, Any]]]:
    """Return (init, image, objects, timeline_lines, crc_rows)."""
    timeline: List[str] = []
    objects: List[DfuObject] = []
    crc_rows: List[Dict[str, Any]] = []
    cur: Optional[DfuObject] = None
    init: Optional[bytes] = None
    image = bytearray()

    for a in att:
        if a.handle == pkt_h and a.tx:
            if cur is not None:
                cur.data += a.payload
            continue
        if a.handle != ctrl_h or not a.payload:
            continue

        p = a.payload
        op = p[0]
        name = OP_NAME.get(op, f"OP_0x{op:02x}")

        if a.tx and op == DFU_SELECT and len(p) >= 2:
            timeline.append(
                f"rec={a.rec:6d} TX SELECT type={p[1]} ({'command' if p[1]==1 else 'data' if p[1]==2 else '?'})"
            )
        elif a.tx and op == DFU_SET_PRN:
            prn = struct.unpack_from("<H", p, 1)[0] if len(p) >= 3 else -1
            timeline.append(f"rec={a.rec:6d} TX SET_PRN {prn}")
        elif a.tx and op == DFU_CREATE and len(p) >= 6:
            typ = p[1]
            size = struct.unpack_from("<I", p, 2)[0]
            cur = DfuObject(obj_type=typ, declared_size=size, create_rec=a.rec)
            timeline.append(
                f"rec={a.rec:6d} TX CREATE type={typ} size={size}"
            )
        elif a.tx and op == DFU_CALC_CRC:
            timeline.append(f"rec={a.rec:6d} TX CALC_CRC")
        elif a.tx and op == DFU_EXECUTE:
            timeline.append(f"rec={a.rec:6d} TX EXECUTE")
            if cur is not None and cur.device_crc is not None:
                objects.append(cur)
            cur = None
        elif (not a.tx) and op == DFU_RESPONSE and len(p) >= 3:
            req, result = p[1], p[2]
            rest = p[3:]
            extra = ""
            if req == DFU_CALC_CRC and result == 1 and len(rest) >= 8 and cur is not None:
                offset, crc = struct.unpack_from("<II", rest, 0)
                data = bytes(cur.data)
                if cur.obj_type == OBJ_COMMAND:
                    init = data
                    local = zlib.crc32(data) & 0xFFFFFFFF
                    scope_len = len(data)
                else:
                    # Data CRC responses are cumulative over the whole image.
                    image.extend(data)
                    local = zlib.crc32(image) & 0xFFFFFFFF
                    scope_len = len(image)
                cur.device_offset = offset
                cur.device_crc = crc
                cur.local_crc = local
                cur.crc_ok = crc == local and offset == scope_len
                extra = (
                    f" offset={offset} crc=0x{crc:08x} local=0x{local:08x} "
                    f"ok={cur.crc_ok} got={len(data)}/{cur.declared_size}"
                )
                crc_rows.append(
                    {
                        "rec": a.rec,
                        "obj_type": cur.obj_type,
                        "declared_size": cur.declared_size,
                        "got": len(data),
                        "device_offset": offset,
                        "device_crc": f"0x{crc:08x}",
                        "local_crc": f"0x{local:08x}",
                        "crc_ok": cur.crc_ok,
                        "create_rec": cur.create_rec,
                    }
                )
            elif req == DFU_SELECT and result == 1 and len(rest) >= 12:
                max_size, offset, crc = struct.unpack_from("<III", rest, 0)
                extra = f" max={max_size} offset={offset} crc=0x{crc:08x}"
            timeline.append(
                f"rec={a.rec:6d} RX RESP to={OP_NAME.get(req, f'0x{req:02x}')} "
                f"result={result}{extra}"
            )
        else:
            direction = "TX" if a.tx else "RX"
            timeline.append(
                f"rec={a.rec:6d} {direction} {name} {p[:16].hex()}{'…' if len(p)>16 else ''}"
            )

    if cur is not None and cur.data and cur not in objects:
        objects.append(cur)

    return init, bytes(image), objects, timeline, crc_rows


def write_dfu_zip(zip_path: Path, init: bytes, image: bytes) -> None:
    """Write a minimal Nordic-style DFU zip (manifest + .dat + .bin)."""
    manifest = {
        "manifest": {
            "application": {
                "bin_file": "application.bin",
                "dat_file": "application.dat",
            }
        }
    }
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("manifest.json", json.dumps(manifest, indent=2) + "\n")
        zf.writestr("application.dat", init)
        zf.writestr("application.bin", image)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", type=Path, help="btsnoop_hci.log (or combined .last+current)")
    ap.add_argument("-o", "--out", type=Path, required=True, help="output directory")
    ap.add_argument("--ctrl-handle", type=lambda s: int(s, 0), default=None)
    ap.add_argument("--packet-handle", type=lambda s: int(s, 0), default=None)
    args = ap.parse_args()

    if not args.log.is_file():
        print(f"error: missing log {args.log}", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    print(f"Reading {args.log} ({args.log.stat().st_size} bytes)")
    att = iter_btsnoop_att(args.log)
    print(f"ATT value ops: {len(att)}")

    if args.ctrl_handle is not None and args.packet_handle is not None:
        ctrl_h, pkt_h = args.ctrl_handle, args.packet_handle
    else:
        ctrl_h, pkt_h = detect_dfu_handles(att)
    print(f"DFU control=0x{ctrl_h:04x} packet=0x{pkt_h:04x}")

    init, image, objects, timeline, crc_rows = reconstruct(att, ctrl_h, pkt_h)
    crc_ok_n = sum(1 for r in crc_rows if r["crc_ok"])
    crc_fail_n = len(crc_rows) - crc_ok_n

    if init is None:
        print("error: no init (command) object reconstructed", file=sys.stderr)
        return 1
    if not image:
        print("error: no data object image reconstructed", file=sys.stderr)
        return 1

    init_path = args.out / "r1_dfu_init.dat"
    image_path = args.out / "r1_dfu_application.bin"
    zip_path = args.out / "r1_dfu_application.zip"
    init_path.write_bytes(init)
    image_path.write_bytes(image)
    write_dfu_zip(zip_path, init, image)

    (args.out / "timeline_dfu.txt").write_text("\n".join(timeline) + "\n")
    (args.out / "crc_checks.jsonl").write_text(
        "".join(json.dumps(r) + "\n" for r in crc_rows)
    )

    # Version-ish strings for the README
    ver_hits = []
    for needle in (b"2.2.7", b"2.2.6", b"2.2.5", b"B210", b"[RING]"):
        idx = image.find(needle)
        if idx >= 0:
            frag = image[idx : idx + 32]
            ascii_s = "".join(chr(c) if 32 <= c < 127 else "." for c in frag)
            ver_hits.append({"needle": needle.decode(), "offset": idx, "frag": ascii_s})

    init_decoded: Dict[str, Any] = {}
    try:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from r1_fw_analyze import decode_init_packet  # noqa: WPS433

        init_decoded = decode_init_packet(init)
        (args.out / "r1_dfu_init.json").write_text(
            json.dumps(init_decoded, indent=2) + "\n"
        )
    except Exception as exc:  # noqa: BLE001 — best-effort side file
        init_decoded = {"error": str(exc)}

    image_digest = hashlib.sha256(image).digest()
    image_sha = image_digest.hex()
    image_sha_le = image_digest[::-1].hex()  # nrfutil little-endian form
    init_hash = (init_decoded.get("hash") or {}).get("hash_hex")
    meta = {
        "source": str(args.log.resolve()),
        "ctrl_handle": f"0x{ctrl_h:04x}",
        "packet_handle": f"0x{pkt_h:04x}",
        "init_len": len(init),
        "init_md5": hashlib.md5(init).hexdigest(),
        "init_sha256": hashlib.sha256(init).hexdigest(),
        "image_len": len(image),
        "image_md5": hashlib.md5(image).hexdigest(),
        "image_sha256": image_sha,
        "image_sha256_le": image_sha_le,
        "init_image_hash_match": bool(init_hash) and init_hash == image_sha_le,
        "objects": len(objects),
        "crc_checks": len(crc_rows),
        "crc_ok": crc_ok_n,
        "crc_fail": crc_fail_n,
        "version_hits": ver_hits,
        "init_summary": {
            k: init_decoded.get(k)
            for k in (
                "type",
                "fw_version",
                "hw_version",
                "sd_req_hex",
                "app_size",
                "is_debug",
            )
            if k in init_decoded
        },
        "note": (
            "Observe-only reconstruction. Do not replay otaStart or DFU writes "
            "from Hardware One."
        ),
    }
    (args.out / "r1_dfu_meta.json").write_text(json.dumps(meta, indent=2) + "\n")

    readme = f"""R1 Nordic Secure DFU extract (passive)
======================================
Source : {args.log}
Output : {args.out}

THIS TOOL IS READ-ONLY. Never replay otaStart or DFU traffic.

Handles
-------
Control Point : 0x{ctrl_h:04x}
Packet        : 0x{pkt_h:04x}

Reconstructed
-------------
Init (.dat)   : {len(init)} bytes  md5={meta['init_md5']}
Application   : {len(image)} bytes  md5={meta['image_md5']}
SHA256 (BE)   : {meta['image_sha256']}
SHA256 (LE)   : {meta['image_sha256_le']}  (nrfutil InitCommand.hash form)
Init hash OK  : {meta['init_image_hash_match']}
Objects       : {len(objects)}
CRC checks    : {crc_ok_n}/{len(crc_rows)} ok  (zlib/IEEE; data CRCs are cumulative)

Files
-----
r1_dfu_init.dat / .json    Signed Nordic init packet (+ decoded fields)
r1_dfu_application.bin     Stitched firmware image
r1_dfu_application.zip     Minimal DFU zip (manifest + dat + bin)
timeline_dfu.txt           Control-point session
crc_checks.jsonl           Per-object CRC vs device
r1_dfu_meta.json           Hashes + handle summary

Next: r1_fw_analyze.py --bin …/r1_dfu_application.bin --dat …/r1_dfu_init.dat -o …
"""
    if ver_hits:
        readme += "\nStrings in image\n----------------\n"
        for h in ver_hits:
            readme += f"  @{h['offset']}: {h['frag']!r}\n"
    (args.out / "README_DFU.txt").write_text(readme)

    print(f"Wrote {args.out}")
    print(
        f"Done: init={len(init)} image={len(image)} objects={len(objects)} "
        f"crc_ok={crc_ok_n}/{len(crc_rows)}"
    )
    for h in ver_hits:
        print(f"  string @{h['offset']}: {h['frag']!r}")
    return 0 if crc_fail_n == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
