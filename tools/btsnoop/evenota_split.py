#!/usr/bin/env python3
"""
Split an Even Realities G2 EVENOTA firmware bundle into components.

Offline / read-only. Layout matches jimrandomh/g2flash parse_firmware_segments
(validated against stock CDN images and live HCI FILE_CHECK rebuilds).
See docs/OTA_RESEARCH_FINDINGS_2026-07-31.md and tools/btsnoop/README.md.

For firmware recovered from a phone HCI capture of a live flash, use
ota_extract.py (components) — this tool is for complete EVENOTA .bin files.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path
from typing import Any, Dict, List


def crc32c_msb(buf: bytes) -> int:
    """CRC32C MSB-first, init 0, xorout 0 (g2flash crc32c_msb / glasses END check)."""
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


def parse_firmware_segments(img: bytes) -> List[Dict[str, Any]]:
    if len(img) < 0x40:
        raise ValueError("file too small for EVENOTA header")
    if img[:8] != b"EVENOTA\x00":
        raise ValueError(f"bad magic {img[:8]!r}; expected EVENOTA\\0")
    n = struct.unpack_from("<I", img, 8)[0]
    if not (0 < n <= 64):
        raise ValueError(f"implausible component count {n}")

    date = img[16:32].split(b"\0", 1)[0].decode("latin1", errors="replace")
    time_s = img[32:48].split(b"\0", 1)[0].decode("latin1", errors="replace")
    ver = img[48:64].split(b"\0", 1)[0].decode("latin1", errors="replace")

    segs: List[Dict[str, Any]] = []
    for i in range(n):
        eid, off, size, crc = struct.unpack_from("<IIII", img, 0x40 + i * 16)
        if off + 128 > len(img):
            raise ValueError(f"segment {i} subheader past EOF (off=0x{off:x})")
        sub = img[off : off + 128]
        ps = struct.unpack_from("<I", sub, 8)[0]
        sub_crc = struct.unpack_from("<I", sub, 12)[0]
        fn = sub[48:128].split(b"\0", 1)[0].decode("latin1", errors="replace")
        payload_off = off + 128
        if payload_off + ps > len(img):
            raise ValueError(
                f"segment {i} ({fn!r}) payload past EOF "
                f"(off=0x{off:x} ps={ps} file={len(img)})"
            )
        payload = img[payload_off : payload_off + ps]
        calc = crc32c_msb(payload)
        segs.append(
            {
                "index": i,
                "eid": eid,
                "file_off": off,
                "toc_size": size,
                "toc_crc": crc,
                "sub_crc": sub_crc,
                "payload_size": ps,
                "payload_off": payload_off,
                "filename": fn or f"component_{i}_eid{eid}.bin",
                "crc_ok": calc == crc and calc == sub_crc,
                "crc_calc": calc,
                "date": date,
                "time": time_s,
                "version": ver,
            }
        )
    return segs


def safe_name(fn: str, index: int) -> str:
    base = fn.replace("\\", "/").split("/")[-1] or f"component_{index}.bin"
    # keep it filesystem-safe
    out = "".join(c if c.isalnum() or c in "._-+" else "_" for c in base)
    return out or f"component_{index}.bin"


def split_image(img_path: Path, out_dir: Path, force: bool = False) -> List[Dict[str, Any]]:
    img = img_path.read_bytes()
    segs = parse_firmware_segments(img)
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest = {
        "source": str(img_path),
        "size": len(img),
        "magic": "EVENOTA",
        "component_count": len(segs),
        "version": segs[0]["version"] if segs else "",
        "date": segs[0]["date"] if segs else "",
        "time": segs[0]["time"] if segs else "",
        "components": [],
    }

    for s in segs:
        name = f"{s['index']:02d}_{safe_name(s['filename'], s['index'])}"
        payload_path = out_dir / name
        sub_path = out_dir / f"{s['index']:02d}_subheader.bin"
        if payload_path.exists() and not force:
            raise FileExistsError(f"refusing to overwrite {payload_path} (use --force)")
        payload = img[s["payload_off"] : s["payload_off"] + s["payload_size"]]
        sub = img[s["file_off"] : s["file_off"] + 128]
        payload_path.write_bytes(payload)
        sub_path.write_bytes(sub)
        entry = {
            **{k: s[k] for k in (
                "index", "eid", "filename", "file_off", "payload_off",
                "payload_size", "toc_size", "toc_crc", "sub_crc", "crc_calc", "crc_ok",
            )},
            "out_payload": name,
            "out_subheader": sub_path.name,
        }
        # hex crc fields for readability
        for k in ("toc_crc", "sub_crc", "crc_calc"):
            entry[k] = f"0x{s[k]:08x}"
        manifest["components"].append(entry)

    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    lines = [
        f"EVENOTA split: {img_path}",
        f"version={manifest['version']} date={manifest['date']} time={manifest['time']}",
        f"components={len(segs)}",
        "",
    ]
    for c in manifest["components"]:
        lines.append(
            f"[{c['index']}] eid={c['eid']} crc_ok={c['crc_ok']} "
            f"{c['payload_size']}B -> {c['out_payload']}  ({c['filename']})"
        )
    (out_dir / "README.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return segs


def main(argv: List[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Split a G2 EVENOTA firmware .bin into components.")
    ap.add_argument("image", type=Path, help="path to EVENOTA .bin")
    ap.add_argument(
        "-o",
        "--out",
        type=Path,
        default=None,
        help="output directory (default: <image>_split/)",
    )
    ap.add_argument("--force", action="store_true", help="overwrite existing outputs")
    args = ap.parse_args(argv)

    if not args.image.is_file():
        print(f"error: not found: {args.image}", file=sys.stderr)
        return 1
    out = args.out if args.out else Path(str(args.image) + "_split")
    try:
        segs = split_image(args.image, out, force=args.force)
    except (ValueError, FileExistsError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    bad = [s for s in segs if not s["crc_ok"]]
    print(f"Wrote {len(segs)} components to {out}")
    for s in segs:
        flag = "OK" if s["crc_ok"] else "CRC_FAIL"
        print(f"  [{s['index']}] {flag} {s['payload_size']:8d}B  {s['filename']}")
    if bad:
        print(f"WARNING: {len(bad)} component(s) failed CRC32C check", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
