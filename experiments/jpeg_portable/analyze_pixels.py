#!/usr/bin/env python3
"""Validate complete RGB dumps from the physical codec probe.

Pillow is needed only to read fixture dimensions and make an optional contact
sheet. Hardware is compared with the same device's software decoder, which uses
matching nearest-neighbour chroma upsampling. Pillow's JPEG decoder may choose a
different upsampling filter, so its displayed reference is not an exact oracle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
from PIL import Image, ImageDraw


def fnv1a(data):
    digest = 2166136261
    for value in data:
        digest = ((digest ^ value) * 16777619) & 0xffffffff
    return f'{digest:08x}'


def analyze(log, fixtures):
    text = log.read_text(errors='replace')
    chunks = {}
    hashes = {}
    reported = []
    for line in text.splitlines():
        delta = re.match(r'JPEG_CASE name=(\S+) mode=hardware ok=1 .* max_delta=(\d+) mean_delta=([0-9.]+)', line)
        if delta:
            name, maximum, mean = delta.groups()
            reported.append({'name':name,'max_delta':int(maximum),'mean_delta':float(mean)})
        case = re.match(r'JPEG_CASE name=(\S+) mode=(\S+) ok=1 .* hash=([0-9a-f]+)', line)
        if case:
            name, mode, digest = case.groups()
            hashes[name, mode] = digest
        pixels = re.match(r'JPEG_PIXELS name=(\S+) mode=(\S+) offset=(\d+) hex=([0-9a-f]+)', line)
        if pixels:
            name, mode, offset, hex_data = pixels.groups()
            key = (name, mode)
            data = chunks.setdefault(key, bytearray())
            if int(offset) != len(data):
                raise ValueError(f'{key}: missing, duplicate or unordered pixel chunk')
            data.extend(bytes.fromhex(hex_data))
    if not chunks:
        raise ValueError('No pixel dumps in log')
    images = {}
    records = []
    for (name, mode), data in chunks.items():
        with Image.open(fixtures / f'{name}.jpg') as original:
            width, height = original.size
        if len(data) != width * height * 3:
            raise ValueError(f'{name}/{mode}: incomplete RGB output')
        digest = fnv1a(data)
        if hashes.get((name, mode)) != digest:
            raise ValueError(f'{name}/{mode}: dumped pixels disagree with device hash')
        images[name, mode] = Image.frombytes('RGB', (width, height), bytes(data))
        records.append({'name': name, 'mode': mode, 'bytes': len(data), 'fnv1a': digest})
    comparisons = []
    for name in sorted({name for name, mode in chunks}):
        sw = chunks.get((name, 'software'))
        hw = chunks.get((name, 'hardware'))
        if sw is not None and hw is not None:
            differences = [abs(a-b) for a, b in zip(sw, hw)]
            comparisons.append({'name': name, 'channels': len(differences),
                                'max_delta': max(differences),
                                'mean_delta': sum(differences)/len(differences),
                                'channels_above_8': sum(x > 8 for x in differences)})
    return {'log_sha256': hashlib.sha256(log.read_bytes()).hexdigest(),
            'dumps': records, 'hardware_vs_software': comparisons,
            'reported_hardware_vs_software': reported}, images


def contact_sheet(images, fixtures, output):
    names = sorted({name for name, mode in images})
    cell, row = 240, 190
    sheet = Image.new('RGB', (cell*3, 35+row*len(names)), 'white')
    draw = ImageDraw.Draw(sheet)
    for column, label in enumerate(('Fixture (Pillow)', 'Device software', 'Device hardware')):
        draw.text((column*cell+10, 10), label, fill='black')
    for index, name in enumerate(names):
        y = 35+index*row
        ref = Image.open(fixtures / f'{name}.jpg').convert('RGB')
        for column, image in enumerate((ref, images.get((name,'software')), images.get((name,'hardware')))):
            draw.text((column*cell+10,y+3), name, fill='black')
            if image:
                image = image.copy()
                image.thumbnail((210,155), Image.Resampling.NEAREST)
                scale = min(210//image.width,155//image.height)
                if scale > 1: image = image.resize((image.width*scale,image.height*scale),Image.Resampling.NEAREST)
                sheet.paste(image,(column*cell+10,y+23))
    sheet.save(output)


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('log', type=Path)
    p.add_argument('--fixtures', type=Path, default=Path(__file__).resolve().parents[2]/'components/hardwareone/test/host/jpeg_fixtures')
    p.add_argument('--json', type=Path)
    p.add_argument('--contact-sheet', type=Path)
    p.add_argument('--max-delta', type=int, default=8, help='Maximum allowed hardware/software channel difference')
    p.add_argument('--mean-delta', type=float, default=2, help='Maximum allowed mean hardware/software channel difference')
    p.add_argument('--require-hardware', action='store_true')
    a=p.parse_args()
    result, images=analyze(a.log,a.fixtures)
    comparisons=result['hardware_vs_software']
    result['acceptance']={'max_delta': a.max_delta, 'mean_delta': a.mean_delta,
                          'hardware_required': a.require_hardware,
                          'passed': (bool(comparisons) or not a.require_hardware) and all(
                              x['max_delta'] <= a.max_delta and x['mean_delta'] <= a.mean_delta
                              for x in comparisons + result['reported_hardware_vs_software'])}
    if a.contact_sheet: contact_sheet(images,a.fixtures,a.contact_sheet)
    output=json.dumps(result,indent=2)+'\n'
    if a.json: a.json.write_text(output)
    print(output,end='')
    if not result['acceptance']['passed']: raise SystemExit(1)


if __name__=='__main__': main()
