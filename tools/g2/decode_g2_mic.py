#!/usr/bin/env python3
"""
Decode raw G2 mic packets captured by `g2micrec` to WAV.

The G2 firmware emits 205-byte BLE notifications on the LEFT temple's
6402 char while the audio stream is active. The byte layout inside each
packet is unconfirmed — g2-kit-unofficial speculates "5×40 B LC3 frames
+ 5 B header" but never verified. This script:

  1. Hex-dumps the first few packets so we can eyeball any header.
  2. Tries a sweep of plausible LC3 framings (different bitrates,
     frame durations, frames-per-packet, header positions).
  3. Writes a .wav per candidate; you listen and pick one.

NOTE on "decoded cleanly": liblc3 does NOT validate bitstream contents
— it produces PCM for any 40-byte input. The "ok" count is just the
absence of API-level errors. Trust your ears, not the percentage.

Requires:
  - Homebrew `liblc3`  (`brew install liblc3`).
  - Python 3.8+, stdlib only.

Usage:
  python3 decode_g2_mic.py /path/to/g2_mic-XXXX.lc3 [--out-dir DIR]
"""
from __future__ import annotations

import argparse
import glob
import sys
import wave
from collections import Counter
from pathlib import Path

PACKET_BYTES = 205


# ─── liblc3 binding loader ─────────────────────────────────────────────

def find_lc3_module():
    candidates = sorted(glob.glob(
        "/opt/homebrew/Cellar/liblc3/*/opt/homebrew/lib/python*/site-packages"
    ), reverse=True)
    candidates += sorted(glob.glob(
        "/usr/local/Cellar/liblc3/*/opt/homebrew/lib/python*/site-packages"
    ), reverse=True)
    for c in candidates:
        if Path(c, "lc3.py").exists():
            sys.path.insert(0, c)
            try:
                import lc3  # type: ignore
                return lc3
            except ImportError:
                continue
    sys.stderr.write(
        "ERROR: liblc3 Python binding not found. `brew install liblc3` first.\n"
    )
    sys.exit(1)


def find_lc3_dylib() -> str | None:
    for cellar in ("/opt/homebrew/Cellar/liblc3", "/usr/local/Cellar/liblc3"):
        matches = sorted(glob.glob(f"{cellar}/*/lib/liblc3.dylib"), reverse=True)
        if matches:
            return matches[0]
    return None


# ─── packet split + raw-byte analysis ─────────────────────────────────

def split_packets(raw: bytes) -> list[bytes]:
    if len(raw) % PACKET_BYTES != 0:
        sys.stderr.write(
            f"warning: input length {len(raw)} not a multiple of "
            f"{PACKET_BYTES}; truncating last partial packet\n"
        )
    n = len(raw) // PACKET_BYTES
    return [raw[i * PACKET_BYTES : (i + 1) * PACKET_BYTES] for i in range(n)]


def hex_row(b: bytes, width: int = 32) -> str:
    return " ".join(f"{x:02X}" for x in b[:width]) + (" …" if len(b) > width else "")


def analyse_packets(packets: list[bytes]) -> None:
    """Print structure-finding stats. We're looking for any column that
    has low entropy (suggests a header field) vs high entropy (codec
    payload)."""
    print("─── first 5 packets, first 32 bytes ───")
    for i, pkt in enumerate(packets[:5]):
        print(f"  pkt {i:3d}: {hex_row(pkt)}")
    print()

    # Per-column entropy across all packets — find which positions are
    # consistent (header) vs random (payload).
    n_cols = PACKET_BYTES
    col_counts = [Counter() for _ in range(n_cols)]
    for pkt in packets:
        for i, b in enumerate(pkt):
            col_counts[i][b] += 1

    def col_entropy(c: Counter) -> float:
        total = sum(c.values())
        if total == 0:
            return 0.0
        from math import log2
        return -sum((n / total) * log2(n / total) for n in c.values())

    entropies = [col_entropy(c) for c in col_counts]
    # 8 bits ≈ 8.0 max entropy. Header-ish columns will be < 4.
    interesting = [(i, e) for i, e in enumerate(entropies) if e < 5.0]
    print(f"─── columns with entropy < 5 bits (likely header fields) ───")
    if interesting:
        for i, e in interesting[:30]:
            top = col_counts[i].most_common(3)
            top_str = ", ".join(f"0x{v:02X}×{n}" for v, n in top)
            print(f"  col {i:3d}  entropy={e:4.2f}  top: {top_str}")
        if len(interesting) > 30:
            print(f"  ... and {len(interesting) - 30} more low-entropy columns")
    else:
        print("  (none — every column is high-entropy; likely encrypted or "
              "fully payload)")
    print()


# ─── candidate layouts ────────────────────────────────────────────────

def layouts(packets: list[bytes]):
    """Generate (name, frames, frame_us, sample_rate, per_frame_bytes)
    tuples for each candidate decoding."""
    out = []

    def slice_packets(skip_head: int, frame_size: int, n_frames: int,
                      skip_tail: int) -> list[bytes]:
        assert skip_head + frame_size * n_frames + skip_tail == PACKET_BYTES
        frames = []
        for pkt in packets:
            base = skip_head
            for i in range(n_frames):
                frames.append(pkt[base + i * frame_size : base + (i + 1) * frame_size])
        return frames

    # 16 kHz, 10 ms candidates
    out.append(("16k_10ms_5x41_no_hdr",      slice_packets(0, 41, 5, 0),  10000, 16000))
    out.append(("16k_10ms_5x40_5b_head",     slice_packets(5, 40, 5, 0),  10000, 16000))
    out.append(("16k_10ms_5x40_5b_tail",     slice_packets(0, 40, 5, 5),  10000, 16000))
    out.append(("16k_10ms_5x40_4b_head_1tail", slice_packets(4, 40, 5, 1), 10000, 16000))
    out.append(("16k_10ms_5x40_1b_head_4tail", slice_packets(1, 40, 5, 4), 10000, 16000))
    out.append(("16k_10ms_4x50_5b_head",     slice_packets(5, 50, 4, 0),  10000, 16000))
    out.append(("16k_10ms_4x50_5b_tail",     slice_packets(0, 50, 4, 5),  10000, 16000))
    out.append(("16k_10ms_2x100_5b_head",    slice_packets(5, 100, 2, 0), 10000, 16000))
    out.append(("16k_10ms_1x200_5b_head",    slice_packets(5, 200, 1, 0), 10000, 16000))

    # 16 kHz, 5 ms candidates (10 frames per 50 ms BLE packet)
    out.append(("16k_5ms_10x20_5b_head",     slice_packets(5, 20, 10, 0),  5000, 16000))
    out.append(("16k_5ms_10x20_5b_tail",     slice_packets(0, 20, 10, 5),  5000, 16000))
    # 5 ms also fits 5×41 if header free, with no leftover bytes — but at
    # 5 ms / 16 kHz / 41 B = 65.6 kbps, not a standard rate.

    # 16 kHz, 2.5 ms candidates (20 frames per 50 ms BLE packet)
    out.append(("16k_2.5ms_20x10_5b_head",   slice_packets(5, 10, 20, 0),  2500, 16000))

    # 8 kHz, 10 ms candidates (less likely but possible)
    out.append(("8k_10ms_5x40_5b_head",      slice_packets(5, 40, 5, 0),  10000, 8000))
    out.append(("8k_10ms_4x50_5b_head",      slice_packets(5, 50, 4, 0),  10000, 8000))

    # 24 kHz, 10 ms (mid-rate)
    out.append(("24k_10ms_5x40_5b_head",     slice_packets(5, 40, 5, 0),  10000, 24000))

    return out


def write_wav(path: Path, pcm_le16: bytes, sample_rate: int) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm_le16)


def decode_layout(lc3_mod, libpath, frames, frame_us, sample_rate):
    """Returns (pcm_bytes, ok_count). Catches per-frame errors."""
    try:
        dec = lc3_mod.Decoder(
            frame_duration_us=frame_us,
            sample_rate_hz=sample_rate,
            num_channels=1,
            libpath=libpath,
        )
    except Exception as e:
        return None, str(e)

    samples_per_frame = sample_rate * frame_us // 1_000_000
    silent = b"\x00\x00" * samples_per_frame
    chunks = []
    ok = 0
    for fr in frames:
        try:
            pcm = dec.decode(fr, bit_depth=16)
            if len(pcm) != len(silent):
                pcm = (pcm + silent)[:len(silent)]
            chunks.append(pcm)
            ok += 1
        except Exception:
            chunks.append(silent)
    return b"".join(chunks), ok


# ─── main ─────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", type=Path)
    ap.add_argument("--out-dir", type=Path, default=None)
    args = ap.parse_args()

    if not args.input.exists():
        sys.stderr.write(f"input file not found: {args.input}\n")
        return 2

    lc3 = find_lc3_module()
    libpath = find_lc3_dylib()
    print(f"liblc3 dylib: {libpath or '(system search path)'}\n")

    out_dir = args.out_dir or args.input.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    raw = args.input.read_bytes()
    packets = split_packets(raw)
    print(f"input: {args.input.name}  ({len(raw)} B → {len(packets)} packets, "
          f"{len(packets) * 0.05:.2f} s of audio @ 50 ms/packet)\n")
    if not packets:
        return 2

    analyse_packets(packets)

    print("─── decoding candidates (listen to each .wav) ───")
    for name, frames, frame_us, sr in layouts(packets):
        per_frame = len(frames[0]) if frames else 0
        bitrate = per_frame * 8 * (1_000_000 // frame_us)
        pcm, ok = decode_layout(lc3, libpath, frames, frame_us, sr)
        if pcm is None:
            print(f"  {name:40s} skip ({ok})")
            continue
        wav_path = out_dir / f"{name}.wav"
        write_wav(wav_path, pcm, sr)
        print(f"  {name:40s} {len(frames):5d} frames × {per_frame:3d}B "
              f"≈{bitrate:6d} bps  → {wav_path.name}")
    print()
    print("If none of these sounds like speech, the audio is probably either:")
    print("  • encrypted (G2 may scramble mic data for privacy)")
    print("  • a non-LC3 codec (Opus / mSBC / proprietary)")
    print("  • framed with a layout we haven't tried (see hex dump above)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
