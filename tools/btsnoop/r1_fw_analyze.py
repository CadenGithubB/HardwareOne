#!/usr/bin/env python3
"""
Offline analysis of an R1 application image + Nordic DFU init packet
(from r1_dfu_extract.py). Read-only. Never flashes or transmits.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from collections import Counter
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

FW_TYPE = {0: "APPLICATION", 1: "SOFTDEVICE", 2: "BOOTLOADER", 3: "SOFTDEVICE_BOOTLOADER"}
HASH_TYPE = {0: "NO_HASH", 1: "CRC", 2: "SHA128", 3: "SHA256", 4: "SHA512"}
SIG_TYPE = {0: "ECDSA_P256_SHA256", 1: "ED25519"}
# dfu-cc.proto ValidationType (nrfutil)
VALIDATION = {
    0: "NO_VALIDATION",
    1: "VALIDATE_GENERATED_CRC",
    2: "VALIDATE_SHA256",
    3: "VALIDATE_ECDSA_P256_SHA256",
}


def read_varint(buf: bytes, i: int) -> Tuple[int, int]:
    x = 0
    s = 0
    while i < len(buf):
        b = buf[i]
        i += 1
        x |= (b & 0x7F) << s
        if not (b & 0x80):
            return x, i
        s += 7
        if s > 63:
            raise ValueError("varint too long")
    raise ValueError("truncated varint")


def decode_fields(buf: bytes) -> List[Tuple[int, int, Any]]:
    i = 0
    out: List[Tuple[int, int, Any]] = []
    while i < len(buf):
        key, i = read_varint(buf, i)
        fn, wt = key >> 3, key & 7
        if wt == 0:
            val, i = read_varint(buf, i)
            out.append((fn, wt, val))
        elif wt == 1:
            out.append((fn, wt, buf[i : i + 8]))
            i += 8
        elif wt == 2:
            ln, i = read_varint(buf, i)
            out.append((fn, wt, buf[i : i + ln]))
            i += ln
        elif wt == 5:
            out.append((fn, wt, buf[i : i + 4]))
            i += 4
        else:
            break
    return out


def decode_packed_u32(buf: bytes) -> List[int]:
    i = 0
    vals: List[int] = []
    while i < len(buf):
        v, i = read_varint(buf, i)
        vals.append(v)
    return vals


def decode_init_packet(init: bytes) -> Dict[str, Any]:
    """Nordic dfu-cc.proto Packet.signed_command → InitCommand (+ ECDSA sig)."""
    top = decode_fields(init)
    if not top or top[0][0] != 2 or top[0][1] != 2:
        raise ValueError("expected Packet.signed_command (field 2 length-delimited)")
    signed = decode_fields(top[0][2])
    by_fn = {fn: val for fn, wt, val in signed}
    if 1 not in by_fn:
        raise ValueError("SignedCommand missing command")
    command = decode_fields(by_fn[1])
    cmd_fn = {fn: val for fn, wt, val in command}
    if cmd_fn.get(1) != 1:
        raise ValueError(f"Command.op_code={cmd_fn.get(1)!r}, expected INIT(1)")
    init_cmd = decode_fields(cmd_fn[2])
    ic: Dict[int, Any] = {fn: val for fn, wt, val in init_cmd}

    hash_info: Dict[str, Any] = {}
    if 8 in ic:
        hf = {fn: val for fn, wt, val in decode_fields(ic[8])}
        hash_info = {
            "hash_type": HASH_TYPE.get(hf.get(1, -1), hf.get(1)),
            "hash_type_id": hf.get(1),
            "hash_hex": bytes(hf.get(2, b"")).hex(),
        }

    boot_vals = []
    # field 10 may appear once; protobuf repeated — scan init_cmd list
    for fn, wt, val in init_cmd:
        if fn != 10 or wt != 2:
            continue
        bf = {a: b for a, _w, b in decode_fields(val)}
        boot_vals.append(
            {
                "type": VALIDATION.get(bf.get(1, -1), bf.get(1)),
                "type_id": bf.get(1),
                "bytes_hex": bytes(bf.get(2, b"")).hex(),
            }
        )

    sd_req = decode_packed_u32(ic[3]) if 3 in ic else []
    fw_type_id = ic.get(4, 0)
    sig = bytes(by_fn.get(3, b""))
    sig_type = by_fn.get(2, 0)

    result = {
        "format": "nordic-dfu-cc SignedCommand/InitCommand",
        "fw_version": ic.get(1),
        "hw_version": ic.get(2),
        "sd_req": sd_req,
        "sd_req_hex": [f"0x{x:04x}" for x in sd_req],
        "type": FW_TYPE.get(fw_type_id, fw_type_id),
        "type_id": fw_type_id,
        "sd_size": ic.get(5, 0),
        "bl_size": ic.get(6, 0),
        "app_size": ic.get(7, 0),
        "hash": hash_info,
        "is_debug": bool(ic.get(9, 0)),
        "boot_validation": boot_vals,
        "signature_type": SIG_TYPE.get(sig_type, sig_type),
        "signature_hex": sig.hex(),
        "init_len": len(init),
    }
    return result


def mine_image(img: bytes) -> Dict[str, Any]:
    strings = [m.group().decode("ascii") for m in re.finditer(rb"[\x20-\x7e]{6,}", img)]
    uniq = list(dict.fromkeys(strings))  # stable unique

    def grab(pred, limit=80):
        return [s for s in uniq if pred(s)][:limit]

    sp, reset = struct.unpack_from("<II", img, 0)
    versions = sorted(
        {s for s in uniq if re.search(r"\d+\.\d+\.\d+", s) and len(s) < 80},
        key=len,
    )
    hw = grab(
        lambda s: any(
            k in s
            for k in (
                "IQS7211",
                "YHM2710",
                "BMA456",
                "Gh3x",
                "gomore",
                "GH_SPO2",
                "603MV",
            )
        )
    )
    symbols = grab(
        lambda s: s.startswith(
            (
                "RING_CTRL_",
                "THREAD_BLE_",
                "proto_",
                "service_",
                "glasses_",
                "ADV_START",
            )
        )
        or s in ("BMA456", "BMA456W")
    )
    ring_logs = grab(lambda s: s.startswith("[RING]"), limit=120)

    # Build path / product
    paths = grab(lambda s: "B210" in s or "_build" in s)

    sha = hashlib.sha256(img).digest()
    return {
        "size": len(img),
        "md5": hashlib.md5(img).hexdigest(),
        "sha256": sha.hex(),
        "vector_sp": f"0x{sp:08x}",
        "vector_reset": f"0x{reset:08x}",
        "looks_like_nrf52": (0x20000000 <= sp <= 0x20040000) and (reset & 1) == 1,
        "version_strings": versions[:40],
        "product_paths": paths,
        "hardware_strings": hw,
        "symbol_strings": symbols,
        "ring_log_sample": ring_logs,
        "ring_log_count": sum(1 for s in strings if s.startswith("[RING]")),
        "string_count": len(strings),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--bin",
        type=Path,
        required=True,
        help="r1_dfu_application.bin",
    )
    ap.add_argument(
        "--dat",
        type=Path,
        default=None,
        help="r1_dfu_init.dat (optional but recommended)",
    )
    ap.add_argument("-o", "--out", type=Path, required=True, help="output directory")
    args = ap.parse_args()

    if not args.bin.is_file():
        print(f"error: missing {args.bin}", file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    img = args.bin.read_bytes()
    report: Dict[str, Any] = {"bin": str(args.bin.resolve()), "image": mine_image(img)}

    if args.dat and args.dat.is_file():
        init = args.dat.read_bytes()
        try:
            decoded = decode_init_packet(init)
        except Exception as e:
            decoded = {"error": str(e)}
        report["init"] = decoded
        # Hash check
        expect_hex = (decoded.get("hash") or {}).get("hash_hex")
        if expect_hex:
            digest = hashlib.sha256(img).digest()
            # nrfutil package.py: return hash in little endian → digest[::-1]
            got_le = digest[::-1].hex()
            got_be = digest.hex()
            report["hash_check"] = {
                "init_sha256_le": expect_hex,
                "image_sha256_be": got_be,
                "image_sha256_le": got_le,
                "match": expect_hex == got_le,
                "app_size_match": decoded.get("app_size") == len(img),
                "note": (
                    "Nordic nrfutil stores InitCommand.hash as SHA-256(image) "
                    "with the 32-byte digest reversed (little-endian)."
                ),
            }

    (args.out / "r1_fw_analysis.json").write_text(json.dumps(report, indent=2) + "\n")

    # Human summary
    img_m = report["image"]
    lines = [
        "R1 firmware offline analysis",
        "============================",
        f"Image : {args.bin} ({img_m['size']} bytes)",
        f"SHA256: {img_m['sha256']}",
        f"Vector: SP={img_m['vector_sp']} Reset={img_m['vector_reset']} "
        f"nRF52-like={img_m['looks_like_nrf52']}",
        "",
        "Versions",
        "--------",
    ]
    for v in img_m["version_strings"][:25]:
        lines.append(f"  {v}")
    lines += ["", "Hardware / stacks", "-----------------"]
    for s in img_m["hardware_strings"]:
        lines.append(f"  {s}")
    lines += ["", "Symbols / tags", "--------------"]
    for s in img_m["symbol_strings"]:
        lines.append(f"  {s}")
    if "init" in report and "error" not in report["init"]:
        ini = report["init"]
        lines += [
            "",
            "Init packet (Nordic SignedCommand)",
            "----------------------------------",
            f"  type       : {ini['type']}",
            f"  fw_version : {ini['fw_version']}",
            f"  hw_version : {ini['hw_version']}",
            f"  sd_req     : {ini['sd_req_hex']}",
            f"  app_size   : {ini['app_size']}",
            f"  bl/sd size : {ini['bl_size']}/{ini['sd_size']}",
            f"  is_debug   : {ini['is_debug']}",
            f"  hash       : {ini['hash']}",
            f"  boot_val   : {ini['boot_validation']}",
            f"  sig_type   : {ini['signature_type']}",
        ]
    if "hash_check" in report:
        hc = report["hash_check"]
        lines += [
            "",
            f"Hash check   : match={hc['match']} app_size_match={hc['app_size_match']}",
            f"  init hash (LE) : {hc['init_sha256_le']}",
            f"  image SHA256   : {hc['image_sha256_be']}",
            f"  image SHA256 LE: {hc['image_sha256_le']}",
            f"  note: {hc['note']}",
        ]
    lines += [
        "",
        "See also: r1_fw_analysis.json, strings_mine.txt (if present).",
        "SAFETY: observe-only — do not flash or replay DFU/otaStart from our stack.",
    ]
    text = "\n".join(lines) + "\n"
    (args.out / "r1_fw_analysis.txt").write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
