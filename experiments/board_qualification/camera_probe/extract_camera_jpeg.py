#!/usr/bin/env python3
"""Validate and extract a camera-probe JPEG. Keep logs and images in private/."""
import argparse
import hashlib
import io
import json
import re
import zlib
from pathlib import Path

from PIL import Image, ImageStat


def decode_capture(text):
    starts = list(re.finditer(r'CAM_JPEG_BEGIN bytes=(\d+) crc32=([0-9a-fA-F]+)', text))
    if len(starts) != 1:
        raise ValueError(f'expected exactly one JPEG, got {len(starts)}')
    start = starts[0]
    length, expected_crc = int(start[1]), int(start[2], 16)
    if not 0 < length <= 8 * 1024 * 1024:
        raise ValueError('unreasonable JPEG length')
    end = text.find('CAM_JPEG_END', start.end())
    if end < 0:
        raise ValueError('missing JPEG end marker')
    data = bytearray()
    chunks = 0
    for match in re.finditer(r'CAM_JPEG offset=(\d+) hex=([0-9a-fA-F]+)', text[start.end():end]):
        offset, chunk = int(match[1]), bytes.fromhex(match[2])
        if offset != len(data) or len(chunk) > 128:
            raise ValueError(f'bad chunk offset={offset}, expected={len(data)}, bytes={len(chunk)}')
        data.extend(chunk)
        chunks += 1
    if len(data) != length:
        raise ValueError(f'length mismatch {len(data)} != {length}')
    crc = zlib.crc32(data)
    if crc != expected_crc:
        raise ValueError(f'CRC mismatch {crc:08x} != {expected_crc:08x}')
    with Image.open(io.BytesIO(data)) as image:
        if image.format != 'JPEG':
            raise ValueError('not JPEG')
        image.load()
        rgb = image.convert('RGB')
        stats = ImageStat.Stat(rgb)
        result = {
            'bytes': len(data), 'chunks': chunks, 'crc32': f'{crc:08x}',
            'sha256': hashlib.sha256(data).hexdigest(), 'format': image.format,
            'width': image.width, 'height': image.height, 'mode': image.mode,
            'channel_mean': stats.mean, 'channel_stddev': stats.stddev,
            'channel_extrema': rgb.getextrema(),
        }
    return bytes(data), result


def extract(log, output):
    metadata = output.with_suffix('.json')
    if output.exists() or metadata.exists():
        raise ValueError('output or metadata already exists; use a fresh path')
    data, result = decode_capture(log.read_text(errors='replace'))
    with output.open('xb') as stream:
        stream.write(data)
    with metadata.open('x') as stream:
        stream.write(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(extract(args.log, args.output), indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
