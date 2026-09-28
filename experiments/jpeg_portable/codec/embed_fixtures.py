#!/usr/bin/env python3
"""Embed the committed, original synthetic JPEG corpus for standalone firmware."""
from pathlib import Path
import re
import sys
source, destination = map(Path, sys.argv[1:])
lines = ['#pragma once', '#include <stddef.h>', '#include <stdint.h>',
         'struct JpegFixture { const char* name; const uint8_t* bytes; size_t size; bool decodable; };']
entries = []
for file in sorted(source.glob('*.jpg')):
    symbol = 'fixture_' + re.sub(r'[^a-zA-Z0-9_]', '_', file.stem)
    data = file.read_bytes()
    lines.append('static const uint8_t ' + symbol + '[] = {')
    for start in range(0, len(data), 20):
        lines.append(','.join(str(x) for x in data[start:start+20]) + ',')
    lines.append('};')
    entries.append('{"%s", %s, sizeof(%s), %s},' %
                   (file.stem, symbol, symbol, 'false' if file.stem.startswith('progressive') else 'true'))
if not entries:
    raise SystemExit('No JPEG fixtures found')
lines.append('static const JpegFixture kFixtures[] = {')
lines.extend(entries)
lines.append('};')
lines.append('static constexpr size_t kFixtureCount = sizeof(kFixtures) / sizeof(kFixtures[0]);')
destination.write_text('\n'.join(lines) + '\n')
