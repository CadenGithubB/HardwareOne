#!/usr/bin/env python3
"""ESP-NOW V4 header flags (1.0 rules), checked on the production sources.

System_ESPNow_Wire.h defines two classes of header flag: bits in 0xF000 are
must-understand (a receiver drops a frame carrying one it does not know), every
other bit is ignored by a receiver that does not know it. This lint fails when:

- a flag removed at 1.0 because nothing set it or nothing read it (ENCRYPTED,
  COMPRESS, STREAM_BEGIN, STREAM_END, HANDSHAKE, PRIORITY_HIGH) is defined or
  named again in the firmware sources;
- a defined flag (ESPNOW_V4_FLAG_*) is not both set and read in
  components/hardwareone/*.cpp (comments ignored);
- the must-understand class is not 0xF000, a 1.0 flag lies inside it without
  being listed as known, or the "unknown" mask is not class minus known;
- the receive front door (v4_try_handle_incoming) does not drop and count a
  frame with an unknown must-understand bit, after the CRC check, the mesh
  gate and the own-origin guard and before the BROADCAST_AUTH block (so before
  ACK, duplicate ring, authentication and flood forwarding);
- any source sets a must-understand bit (no 1.0 frame may carry one).

The behaviour itself (drop, no ACK, no duplicate entry, counter, routed frames)
is run by test_espnow_rx_frontend.py.
"""
import argparse
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

REMOVED = ('ENCRYPTED', 'COMPRESS', 'STREAM_BEGIN', 'STREAM_END', 'HANDSHAKE', 'PRIORITY_HIGH')


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    comp = a.component
    failures = []

    wire = (comp / 'System_ESPNow_Wire.h').read_text()
    enum = extract_block(wire, 'enum EspNowV4Flags')
    members = dict((m.group(1), m.group(2)) for m in
                   re.finditer(r'\b(ESPNOW_V4_FLAGS?_\w+)\s*=\s*([^,\n]+?)\s*,', strip_comments(enum)))
    flags = {k: int(v, 0) for k, v in members.items() if k.startswith('ESPNOW_V4_FLAG_')}
    masks = {k: v for k, v in members.items() if k.startswith('ESPNOW_V4_FLAGS_')}

    for name in REMOVED:
        if f'ESPNOW_V4_FLAG_{name}' in members:
            failures.append(f'ESPNOW_V4_FLAG_{name} is defined again (removed at 1.0: never set or never read)')
    for path in sorted(list(comp.glob('*.cpp')) + list(comp.glob('*.h'))):
        code = strip_comments(path.read_text(errors='replace'))
        for name in REMOVED:
            if re.search(r'\bESPNOW_V4_FLAG_' + name + r'\b', code):
                failures.append(f'{path.name} uses ESPNOW_V4_FLAG_{name} (removed at 1.0)')

    try:
        must = int(masks['ESPNOW_V4_FLAGS_MUST_UNDERSTAND'], 0)
        known = int(masks['ESPNOW_V4_FLAGS_KNOWN_MUST_UNDERSTAND'], 0)
    except (KeyError, ValueError):
        failures.append('the enum has no ESPNOW_V4_FLAGS_MUST_UNDERSTAND / _KNOWN_MUST_UNDERSTAND number')
        must, known = 0xF000, 0
    if must != 0xF000:
        failures.append(f'the must-understand class is 0x{must:04X}, not 0xF000')
    if known & ~must:
        failures.append('a known must-understand bit lies outside the class')
    unknown = masks.get('ESPNOW_V4_FLAGS_UNKNOWN_MUST_UNDERSTAND', '')
    if re.sub(r'\s+', '', unknown) != 'ESPNOW_V4_FLAGS_MUST_UNDERSTAND&~ESPNOW_V4_FLAGS_KNOWN_MUST_UNDERSTAND':
        failures.append('ESPNOW_V4_FLAGS_UNKNOWN_MUST_UNDERSTAND is not the class minus the known bits')
    for name, value in flags.items():
        if value & must and not value & known:
            failures.append(f'{name} (0x{value:04X}) lies in the must-understand class but is not known')

    sources = {path.name: strip_comments(path.read_text(errors='replace'))
               for path in sorted(comp.glob('*.cpp'))}
    for name in flags:
        read = any(re.search(r'&\s*\(?\s*' + name + r'\b', code) for code in sources.values())
        sets = any(re.search(r'(?<![&\w])\s*' + name + r'\b', re.sub(r'&\s*\(?\s*' + name + r'\b', '', code))
                   for code in sources.values())
        if not read:
            failures.append(f'{name} is never read (no "& {name}" in components/hardwareone/*.cpp)')
        if not sets:
            failures.append(f'{name} is never set in components/hardwareone/*.cpp')

    for fname, code in sources.items():
        for m in re.finditer(r'\bESPNOW_V4_FLAGS_\w+', code):
            if fname != 'System_ESPNow.cpp' or m.group(0) != 'ESPNOW_V4_FLAGS_UNKNOWN_MUST_UNDERSTAND':
                failures.append(f'{fname} names {m.group(0)}: only the front door reads the class, nothing sets it')
        if re.search(r'flags\s*\|?=\s*[^;]*0x[1-9A-Fa-f]000\b', code):
            failures.append(f'{fname} seems to set a must-understand bit by number')

    espnow = (comp / 'System_ESPNow.cpp').read_text()
    marker = 'static bool v4_try_handle_incoming('
    incoming = extract_block(espnow[espnow.rindex(marker):], marker)
    body = strip_comments(incoming)
    drop = body.find('h->flags & ESPNOW_V4_FLAGS_UNKNOWN_MUST_UNDERSTAND')
    crc = body.find('v4_crc16_ccitt(')
    mesh = body.find('meshByFingerprint(')
    origin = body.find('isSelfMac(h->origin)')
    bauth = body.find('h->flags & ESPNOW_V4_FLAG_BROADCAST_AUTH')
    if drop < 0:
        failures.append('v4_try_handle_incoming does not test ESPNOW_V4_FLAGS_UNKNOWN_MUST_UNDERSTAND')
    elif not (0 <= crc < drop and 0 <= mesh < drop and 0 <= origin < drop and drop < bauth):
        failures.append('the must-understand drop is not after the CRC check, the mesh gate and the own-origin '
                        'guard and before the BROADCAST_AUTH block')
    else:
        block = body[drop:body.index('return true;', drop)]
        if 'gEspNowRxMustUnderstandDrops++' not in block:
            failures.append('the must-understand drop is not counted (gEspNowRxMustUnderstandDrops)')
    stats = extract_block(espnow, 'const char* cmd_espnow_stats(const String& argsInput)')
    if 'doc["rxMustUnderstandDrops"]' not in stats or 'RX Must-Understand Drops' not in stats:
        failures.append('espnowstats does not show the must-understand drops (text and JSON)')

    for line in failures:
        print(f'FAIL {line}')
    if failures:
        sys.exit(1)
    print(f'PASS espnow_header_flags: {len(flags)} flags, each set and read; class 0x{must:04X}, known 0x{known:04X}')


if __name__ == '__main__':
    main()
