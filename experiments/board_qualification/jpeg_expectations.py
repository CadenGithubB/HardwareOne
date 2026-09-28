#!/usr/bin/env python3
"""Derive full-app diagnostic reference hashes from qualified synthetic pixels.
Reads saved codec logs only; never opens hardware. Full RGB hashes are the
physically qualified references. BMP hashes independently apply the current G2
Linear tone/letterbox specification to complete small-image pixel dumps.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return f'{value:08x}'


def linear_bmp(rgb, width, height):
    assert len(rgb) == width * height * 3
    dstw, dsth, offset = 288, 144, 118
    size = offset + dstw * dsth // 2
    result = bytearray(size)
    struct.pack_into('<2sIHHI', result, 0, b'BM', size, 0, 0, offset)
    struct.pack_into('<IiiHHIIiiII', result, 14, 40, dstw, -dsth, 1, 4, 0,
                     dstw * dsth // 2, 2835, 2835, 16, 0)
    for index in range(16):
        level = index * 255 // 15
        result[54 + 4 * index:58 + 4 * index] = bytes((level, level, level, 0))
    if width * dsth > height * dstw:
        fitw, fith = dstw, height * dstw // width
    else:
        fitw, fith = width * dsth // height, dsth
    offx, offy = (dstw - fitw) // 2, (dsth - fith) // 2
    for y in range(fith):
        sy = y * height // fith
        for x in range(fitw):
            sx = x * width // fitw
            start = (sy * width + sx) * 3
            gray = sum(rgb[start:start + 3]) // 3
            # C++ integer division truncates toward zero, including negatives.
            numerator = (gray - 128) * 352
            contrast = (abs(numerator) // 256) * (-1 if numerator < 0 else 1) + 128
            contrast = max(0, min(255, contrast))
            nibble = (contrast * 15 + 127) // 255
            dx, dy = x + offx, y + offy
            result[offset + dy * (dstw // 2) + dx // 2] |= nibble << (4 if dx % 2 == 0 else 0)
    return bytes(result)


def expectations(log, fixtures):
    data = log.read_bytes()
    frames, records = {}, {}
    for line in data.decode('utf8', 'replace').splitlines():
        case = re.match(r'JPEG_CASE name=(\S+) mode=(software|hardware) ok=(\d) backend=(\S+) us=(\d+) hash=([0-9a-f]+)', line)
        if case:
            name, mode, ok, backend, _, digest = case.groups()
            if ok == '1':
                records.setdefault(name, {})[mode] = {'backend': backend, 'rgb_hash': digest}
        match = re.match(r'JPEG_PIXELS name=(\S+) mode=(\S+) offset=(\d+) hex=([0-9a-f]+)', line)
        if match:
            name, mode, offset, encoded = match.groups()
            rgb = frames.setdefault((name, mode), bytearray())
            assert len(rgb) == int(offset), 'missing/duplicate pixel chunk'
            rgb.extend(bytes.fromhex(encoded))
    for (name, mode), rgb in frames.items():
        width, height = map(int, re.search(r'_(\d+)x(\d+)$', name).groups())
        record = records[name][mode]
        assert fnv(rgb) == record['rgb_hash'], 'pixel dump hash mismatch'
        bmp = linear_bmp(rgb, width, height)
        record.update(width=width, height=height, bmp_hash=fnv(bmp), bmp_bytes=len(bmp),
                      bmp_width=288, bmp_height=-144)
    for name, modes in records.items():
        fixture = fixtures / f'{name}.jpg'
        for record in modes.values():
            record['fixture_sha256'] = hashlib.sha256(fixture.read_bytes()).hexdigest()
    return {'log_sha256': hashlib.sha256(data).hexdigest(), 'fixtures': records}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--logs', type=Path, default=HERE.parent / 'jpeg_portable/private/device-20260928')
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    fixtures = REPO / 'components/hardwareone/test/host/jpeg_fixtures'
    result = {'schema': 1, 'scope': 'Reference expectations, not full-app test results',
              'bmp_reference': 'G2 288x144 top-down 4bpp; aspect fit; Linear contrast 352/256; nearest sampling',
              'boards': {board: expectations(a.logs / f'{board}-probe-2-serial.log', fixtures) for board in ('p4', 's3')}}
    encoded = json.dumps(result, indent=2) + '\n'
    if a.output: a.output.write_text(encoded)
    else: print(encoded, end='')


if __name__ == '__main__':
    main()
