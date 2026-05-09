#!/usr/bin/env python3
"""
Extract GIF frames and write G2-compatible 4bpp indexed BMPs (top-down).

Matches firmware layout in G2_Glasses.cpp buildBmp4bpp():
  - BM header, 40-byte DIB, biBitCount=4, biCompression=0
  - biHeight < 0 (top-down), first pixel row = top of frame
  - 16-entry BGRA palette: gray_i = (i * 255) // 15, A=0
  - Row stride 4-byte aligned; high nibble = left pixel of each pair

Output size defaults to the GIF canvas. Pick a common size with --preset, or
set --width/--height for anything else.

  # from repo root (hardwareone-idf/):
  python3 tools/gif_to_g2_bmps.py slime.gif out/ --preset 64
  ./gif_to_g2_bmps slime.gif out/ --preset 64

  # from tools/ — do NOT prefix tools/ again:
  python3 gif_to_g2_bmps.py slime.gif out/ --preset 64

On-device playback (G2 test menu): put frames on the SD card as
  /sd/g2_icon_animations/<short_name>/frame_00.bmp …
Then Tests → Image → Animated Icons (pack list opens first) → tap a pack row,
or Back → Custom icon packs >> to reopen the list. Web upload: POST
/api/files/upload with path under that same prefix (see docs/G2_PROTOCOL.md).

Install (from repo root): pip install -r tools/requirements-gif.txt
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageOps
except ImportError as e:
    print("Install Pillow:  pip install -r tools/requirements-gif.txt", file=sys.stderr)
    raise SystemExit(1) from e

# Named outputs (G2-friendly). Square presets match live-tile probe sizes.
SIZE_PRESETS: dict[str, tuple[int, int] | None] = {
    "native": None,  # use GIF logical screen (gw×gh)
    "32": (32, 32),
    "64": (64, 64),
    "96": (96, 96),
    "288x144": (288, 144),  # one full lens half-tile (g2bmp / readBmpFromVfs)
}


def _grayscale16_palette_image() -> Image.Image:
    """Mode-P 1×1 image whose palette is the 16 G2 grayscale ramp entries."""
    pal: list[int] = []
    for i in range(16):
        v = (i * 255) // 15
        pal.extend([v, v, v])
    pal.extend([0, 0, 0] * (256 - 16))
    im = Image.new("P", (1, 1))
    im.putpalette(pal)
    return im


def _resize_fit(im: Image.Image, w: int, h: int, fit: str) -> Image.Image:
    if im.mode not in ("RGB", "RGBA"):
        im = im.convert("RGBA")
    if fit == "contain":
        return ImageOps.contain(im, (w, h), method=Image.Resampling.LANCZOS)
    if fit == "cover":
        return ImageOps.fit(im, (w, h), method=Image.Resampling.LANCZOS)
    if fit == "stretch":
        return im.resize((w, h), Image.Resampling.LANCZOS)
    raise ValueError(fit)


def _letterbox_to_size(im: Image.Image, w: int, h: int) -> Image.Image:
    """Contain-style: scale down/up then center on w×h black canvas."""
    contained = ImageOps.contain(im, (w, h), method=Image.Resampling.LANCZOS)
    if contained.mode != "RGBA":
        contained = contained.convert("RGBA")
    canvas = Image.new("RGBA", (w, h), (0, 0, 0, 255))
    x = (w - contained.width) // 2
    y = (h - contained.height) // 2
    canvas.paste(contained, (x, y), contained)
    return canvas


def _prepare_frame_rgba(frame: Image.Image, w: int, h: int, fit: str) -> Image.Image:
    if fit == "contain":
        return _letterbox_to_size(frame, w, h)
    return _resize_fit(frame, w, h, fit)


def _quantize_to_g2_palette(rgb: Image.Image, palette_ref: Image.Image) -> Image.Image:
    """RGB → mode P with indices 0..15 matching G2 gray ramp (Floyd–Steinberg)."""
    try:
        dither = Image.Dither.FLOYDSTEINBERG
    except AttributeError:  # Pillow < 9.1
        dither = Image.FLOYDSTEINBERG
    return rgb.quantize(palette=palette_ref, dither=dither)


def build_bmp4_topdown(indices: list[list[int]], width: int, height: int) -> bytes:
    """
    indices[row][col] in 0..15; top row first.
    """
    if width <= 0 or height <= 0:
        raise ValueError("bad dimensions")
    if len(indices) != height or any(len(r) != width for r in indices):
        raise ValueError("indices shape mismatch")

    aw, ah = width, height
    row_stride = ((aw * 4 + 31) // 32) * 4
    pixel_size = row_stride * ah
    header_size = 14 + 40 + 64
    total = header_size + pixel_size

    out = bytearray(total)
    out[0:2] = b"BM"
    struct.pack_into("<I", out, 2, total)
    struct.pack_into("<HH", out, 6, 0, 0)
    struct.pack_into("<I", out, 10, header_size)

    struct.pack_into("<I", out, 14, 40)
    struct.pack_into("<i", out, 18, width)
    struct.pack_into("<i", out, 22, -height)
    struct.pack_into("<HH", out, 26, 1, 4)
    struct.pack_into("<I", out, 30, 0)
    struct.pack_into("<I", out, 34, pixel_size)
    struct.pack_into("<I", out, 38, 2835)
    struct.pack_into("<I", out, 42, 2835)
    struct.pack_into("<II", out, 46, 16, 0)

    for i in range(16):
        v = (i * 255) // 15
        off = 54 + i * 4
        out[off : off + 4] = bytes([v, v, v, 0])

    pixels = memoryview(out)[header_size:]
    for row in range(ah):
        row_off = row * row_stride
        x = 0
        col = 0
        while col < aw:
            left = indices[row][col] & 0x0F
            if col + 1 < aw:
                right = indices[row][col + 1] & 0x0F
                b = (left << 4) | right
            else:
                b = (left << 4) | (left & 0x0F)
            pixels[row_off + x] = b
            x += 1
            col += 2

    return bytes(out)


def main() -> None:
    p = argparse.ArgumentParser(
        description="GIF → numbered 4bpp BMP frames (G2 / EvenCore image path).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Presets for --preset: native, 32, 64, 96, 288x144 "
        "(mutually exclusive with --width/--height).\n\n"
        "Paths: from repo root use  python3 tools/gif_to_g2_bmps.py  …  "
        "or  ./gif_to_g2_bmps  …\n"
        "If your shell is already in tools/, use  python3 gif_to_g2_bmps.py  …  "
        "(not python3 tools/gif_to_g2_bmps.py).",
    )
    p.add_argument("gif", type=Path, help="Input GIF path")
    p.add_argument(
        "out_dir",
        type=Path,
        nargs="?",
        default=None,
        help="Output folder (created if missing). Use a real writable path, e.g. "
        "./slime_bmps or ~/Desktop/frames — not a doc placeholder. "
        "Default: <gif_stem>_bmps beside the GIF.",
    )
    p.add_argument(
        "--width",
        type=int,
        default=None,
        metavar="PX",
        help="Output width (default: GIF canvas width from the file)",
    )
    p.add_argument(
        "--height",
        type=int,
        default=None,
        metavar="PX",
        help="Output height (default: GIF canvas height from the file)",
    )
    p.add_argument(
        "--preset",
        choices=tuple(SIZE_PRESETS.keys()),
        default=None,
        metavar="NAME",
        help="Fixed output size: native (GIF size), 32, 64, 96, or 288x144 "
        "(one lens tile). Cannot combine with --width/--height.",
    )
    p.add_argument(
        "--fit",
        choices=("contain", "cover", "stretch"),
        default="contain",
        help="How to map GIF frame into WxH (default contain on black)",
    )
    p.add_argument(
        "--prefix",
        default="frame_",
        help="Output filename prefix (default frame_) → frame_00.bmp …",
    )
    p.add_argument(
        "--expect-frames",
        type=int,
        default=16,
        metavar="N",
        help="If frame count ≠ N, print a warning (default 16). Use 0 to disable.",
    )
    args = p.parse_args()

    gif_path: Path = args.gif
    if not gif_path.is_file():
        print(f"Not found: {gif_path}", file=sys.stderr)
        sys.exit(1)

    out_dir = args.out_dir if args.out_dir is not None else gif_path.with_name(f"{gif_path.stem}_bmps")
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
    except OSError as exc:
        print(
            f"Cannot create output directory:\n  {out_dir}\n"
            f"({exc})\n\n"
            "Pass a directory you can write to. Examples:\n"
            "  ./slime_bmps\n"
            "  ~/Desktop/slime_bmps\n"
            "Omit the second argument to write next to the GIF:\n"
            f"  python3 gif_to_g2_bmps.py {gif_path.name} --preset 64\n",
            file=sys.stderr,
        )
        sys.exit(1)

    pal_ref = _grayscale16_palette_image()

    im = Image.open(gif_path)
    # Pillow's ImageSequence.Iterator often yields the *same* Image object
    # each time; materializing without .copy() leaves every slot pointing at
    # the last frame. Snapshot each frame after seek.
    n_frames = getattr(im, "n_frames", 1)
    frames: list[Image.Image] = []
    for i in range(n_frames):
        im.seek(i)
        frames.append(im.copy())
    n = len(frames)
    if args.expect_frames and n != args.expect_frames:
        print(
            f"Warning: GIF has {n} frames, expected {args.expect_frames}. "
            f"Writing all {n} as BMP anyway.",
            file=sys.stderr,
        )

    gw, gh = im.size

    if args.preset is not None and (
        args.width is not None or args.height is not None
    ):
        print("Use either --preset or --width/--height, not both.", file=sys.stderr)
        sys.exit(1)

    if args.preset is not None:
        dims = SIZE_PRESETS[args.preset]
        if dims is None:
            w, h = gw, gh
        else:
            w, h = dims
    elif args.width is None and args.height is None:
        w, h = gw, gh
    elif args.width is not None and args.height is not None:
        w, h = args.width, args.height
    else:
        print(
            "Pass both --width and --height, or neither (native GIF canvas), "
            "or use --preset.",
            file=sys.stderr,
        )
        sys.exit(1)

    if (w, h) != (gw, gh):
        print(f"Note: scaling GIF canvas {gw}×{gh} → output {w}×{h}.", file=sys.stderr)
    if w % 2 != 0:
        print("Warning: odd width — BMP 4bpp packs 2 px/byte; using width as-is.", file=sys.stderr)

    canvas_sz = im.size
    for i, frame in enumerate(frames):
        rgba = frame.convert("RGBA")
        if rgba.size != canvas_sz:
            layer = Image.new("RGBA", canvas_sz, (0, 0, 0, 255))
            layer.paste(rgba, (0, 0), rgba)
            rgba = layer
        # Full-frame GIFs: each layer composited on black. (Delta GIFs with
        # disposal may need a smarter compositor — re-export as full frames.)
        bg = Image.new("RGBA", canvas_sz, (0, 0, 0, 255))
        composed = Image.alpha_composite(bg, rgba)

        staged = _prepare_frame_rgba(composed, w, h, args.fit)
        rgb = staged.convert("RGB")
        q = _quantize_to_g2_palette(rgb, pal_ref)

        idx_rows: list[list[int]] = []
        for y in range(h):
            row = [int(q.getpixel((x, y))) for x in range(w)]
            idx_rows.append(row)

        bmp = build_bmp4_topdown(idx_rows, w, h)
        name = f"{args.prefix}{i:02d}.bmp"
        out_path = out_dir / name
        out_path.write_bytes(bmp)
        print(out_path)

    print(f"Wrote {n} BMP(s) to {out_dir} ({w}×{h}, 4bpp, top-down, 16 gray).", file=sys.stderr)


if __name__ == "__main__":
    main()
