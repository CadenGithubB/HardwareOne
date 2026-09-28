#!/usr/bin/env python3
"""Regenerate original synthetic JPEG fixtures with Pillow (tested with 12.3.0)."""
from pathlib import Path
from PIL import Image

OUT = Path(__file__).resolve().parent

def pattern(w, h):
    image = Image.new('RGB', (w, h))
    image.putdata([((x * 17 + y * 3) % 256,
                    (x * 5 + y * 19) % 256,
                    (x * 23 + y * 7) % 256)
                   for y in range(h) for x in range(w)])
    return image

for size in ((17, 19), (24, 24), (320, 240)):
    for sampling in (0, 1, 2):
        label = {0: '444', 1: '422', 2: '420'}[sampling]
        pattern(*size).save(OUT / f'rgb{label}_{size[0]}x{size[1]}.jpg',
                            quality=91, subsampling=sampling, optimize=False)
pattern(17, 19).convert('L').save(OUT / 'gray_17x19.jpg', quality=91)
pattern(17, 19).save(OUT / 'progressive_17x19.jpg', quality=91, progressive=True)
for name, color in (('red', (255, 0, 0)), ('blue', (0, 0, 255))):
    Image.new('RGB', (8, 8), color).save(OUT / f'{name}_8x8.jpg',
                                       quality=100, subsampling=0)

# Wider-than-DMA-block fixtures exercise P4 4:4:4 hardware and channel order.
pattern(48, 48).save(OUT / 'rgb444_48x48.jpg', quality=91, subsampling=0, optimize=False)
for name, color in (('red', (255, 0, 0)), ('blue', (0, 0, 255))):
    Image.new('RGB', (48, 48), color).save(OUT / f'{name}_48x48.jpg',
                                         quality=100, subsampling=0)

# Small solid files exercise the Edge Impulse VGA capacity boundary and HD rejection.
for width, height in ((640, 480), (1280, 720)):
    Image.new('RGB', (width, height), (40, 100, 160)).save(
        OUT / f'solid_{width}x{height}.jpg', quality=85, subsampling=2)
