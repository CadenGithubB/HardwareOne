#!/usr/bin/env python3
"""Inter-device sensor JSON keys have one owner (2026-11-14 sweep, step 4, D3).

A sensor driver writes its reading as hand-built JSON text (snprintf); ESP-NOW
carries it verbatim to the controller, which parses it again and reads fields
by name: getRemoteGpsData (System_ESPNow_Sensors.cpp) and the remote-sensor
OLED screen (displayRemoteSensors, OLED_Mode_Network.cpp). Each key used to be
spelled twice, once in the sender's format string and once at the reader, so a
rename on one side silently zeroed the reading on the other.

Owner: System_SensorJsonKeys.h (HW1_SK_* string-literal macros, HW1_SK_Q(key)
for a quoted key and its colon inside a format string).

Source contract:
- the owner defines exactly the pinned key names and HW1_SK_Q;
- every sender (the seven <x>BuildDataJson functions whose output a receiver
  reads by name) and every receiver includes the owner, spells none of its
  keys and uses the owner's name the pinned number of times;
- the most distinctive keys (sats, ori, yaw, distance_mm, station) appear
  nowhere else in the firmware sources, quoted or as a subscript.

Value check (no behaviour change): every snprintf format of the senders and
every key the receivers index with are extracted from production source,
compiled on the host against the owner, and compared byte for byte with the
values the old code spelled out (pinned below). Each format is also run
through snprintf, old and new, with the same fixed sample values.

--root <repository> reads another tree (for example `git archive HEAD`); the
pre-change tree must fail the contract and pass the value check.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

OWNER = 'System_SensorJsonKeys.h'
INCLUDE = f'#include "{OWNER}"'
KEYS = {
    'FIX': 'fix', 'QUALITY': 'quality', 'SATS': 'sats', 'LAT': 'lat', 'LON': 'lon',
    'ALT': 'alt', 'SPEED': 'speed',
    'ORI': 'ori', 'YAW': 'yaw', 'PITCH': 'pitch', 'ROLL': 'roll',
    'OBJECTS': 'objects', 'DISTANCE_MM': 'distance_mm', 'STATUS': 'status',
    'FREQUENCY': 'frequency', 'RSSI': 'rssi', 'STATION': 'station',
    'X': 'x', 'Y': 'y', 'BUTTONS': 'buttons', 'POS': 'pos', 'AXIS': 'axis',
}
DISTINCTIVE = ('sats', 'ori', 'yaw', 'distance_mm', 'station')

# (file, definition marker, {owner name: uses of HW1_SK_Q(HW1_SK_<name>)}, old formats in order)
SENDERS = [
    ('i2csensor_pa1010d.cpp', 'int gpsBuildDataJson(',
     {'FIX': 2, 'QUALITY': 2, 'SATS': 2, 'LAT': 2, 'LON': 2, 'ALT': 2, 'SPEED': 2},
     [b',"fix":1,"quality":%d,"sats":%d,"lat":%.6f,"lon":%.6f,"alt":%.2f,"speed":%.2f}',
      b',"fix":0,"quality":0,"sats":%d,"lat":0,"lon":0,"alt":0,"speed":0}']),
    ('i2csensor_bno055.cpp', 'int imuBuildDataJson(',
     {'ORI': 2, 'YAW': 1, 'PITCH': 1, 'ROLL': 1},
     [b',"accel":null,"gyro":null,"ori":null,"temp":null}',
      b',"accel":{"x":%.2f,"y":%.2f,"z":%.2f},"gyro":{"x":%.3f,"y":%.3f,"z":%.3f},'
      b'"ori":{"yaw":%.2f,"pitch":%.2f,"roll":%.2f},"temp":%d}']),
    ('i2csensor_vl53l4cx.cpp', 'int tofBuildDataJson(',
     {'OBJECTS': 3, 'DISTANCE_MM': 1, 'STATUS': 1},
     [b',"objects":[]}', b',"objects":[',
      b'%s{"id":%d,"distance_mm":%d,"status":%d,"valid":%s}', b']}',
      b',"objects":[],"total_objects":0}']),
    ('i2csensor_rda5807.cpp', 'int fmRadioBuildDataJson(',
     {'FREQUENCY': 1, 'RSSI': 1, 'STATION': 1},
     [b',"frequency":%.1f,"volume":%d,"muted":%s,"stereo":%s,"rssi":%d,"headphones":%s,'
      b'"seeking":%s,"station":"%s","radioText":"%s"}']),
    ('i2csensor_seesaw.cpp', 'int gamepadBuildDataJson(',
     {'X': 2, 'Y': 2, 'BUTTONS': 2},
     [b',"x":0,"y":0,"buttons":0}', b',"x":%d,"y":%d,"buttons":%lu}']),
    ('i2csensor_ano_encoder.cpp', 'int anoEncoderBuildDataJson(',
     {'POS': 2, 'AXIS': 2, 'BUTTONS': 2},
     [b',"pos":0,"axis":0,"buttons":0}', b',"pos":%ld,"axis":0,"buttons":%lu}']),
    ('Input_GPIOEncoder.cpp', 'int gpioEncoderBuildDataJson(',
     {'POS': 1, 'AXIS': 1, 'BUTTONS': 1},
     [b'{"connected":false,"valid":false}', b'{"connected":false,"valid":false}',
      b'{"connected":%s,"running":%s,"valid":%s,"ts":%lu,"pos":%ld,"axis":0,"pendingDetents":%d,'
      b'"buttons":%lu,"pressed":%s,"sampleMs":%lu,"auxButtonsEnabled":%s}']),
]
# (file, definition marker, old keys indexed in order)
RECEIVERS = [
    ('System_ESPNow_Sensors.cpp', 'bool getRemoteGpsData(',
     ['fix', 'quality', 'sats', 'lat', 'lon', 'alt', 'speed']),
    ('OLED_Mode_Network.cpp', 'void displayRemoteSensors(',
     ['pos', 'pos', 'buttons', 'x', 'y', 'buttons', 'ori', 'roll', 'pitch', 'yaw',
      'lat', 'lon', 'sats', 'fix', 'objects', 'distance_mm', 'status', 'frequency', 'rssi',
      'station']),
]
SUBSCRIPT = re.compile(r'\b(?:doc|ori|obj)\[\s*("(?:[^"\\]|\\.)*"|HW1_SK_\w+)\s*\]')
CONVERSION = re.compile(rb'%[-+ #0]*\d*(?:\.\d+)?(hh|h|ll|l|z)?([diouxXfFeEgGsc%])')


def strip_comments(text: str) -> str:
    """Drop // and /* */ comments, keeping string and character literals."""
    out, i, n = [], 0, len(text)
    while i < n:
        c, nxt = text[i], text[i + 1:i + 2]
        if c == '/' and nxt == '/':
            j = text.find('\n', i)
            i = n if j < 0 else j
        elif c == '/' and nxt == '*':
            j = text.find('*/', i + 2)
            out.append(' ')
            i = n if j < 0 else j + 2
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def call_args(text: str, start: int) -> list[str]:
    """Top-level arguments of the call whose '(' is at text[start]."""
    args, depth, i, begin = [], 0, start, start + 1
    while i < len(text):
        c = text[i]
        if c in '"\'':
            j = i + 1
            while text[j] != c:
                j += 2 if text[j] == '\\' else 1
            i = j
        elif c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                args.append(text[begin:i].strip())
                return args
        elif c == ',' and depth == 1:
            args.append(text[begin:i].strip())
            begin = i + 1
        i += 1
    raise ValueError('unterminated call')


def formats(block: str) -> list[str]:
    return [call_args(block, m.end() - 1)[2] for m in re.finditer(r'\bsnprintf\s*\(', block)]


def sources(component: Path):
    for path in sorted(component.rglob('*')):
        if path.suffix in {'.c', '.cpp', '.h', '.hpp', '.inc'} and path.is_file() and \
                'test' not in path.relative_to(component).parts[:1]:
            yield path


def contract(component: Path) -> list[str]:
    failures: list[str] = []
    owner = component / OWNER
    if not owner.exists():
        failures.append(f'{OWNER} (owner of the inter-device sensor JSON keys) is missing')
    else:
        text = owner.read_text()
        found = dict(re.findall(r'^#define HW1_SK_(\w+)\s+"([^"]*)"\s*$', text, re.M))
        if found != KEYS:
            failures.append(f'{OWNER} must own exactly the pinned keys; got {found}')
        if len(re.findall(r'^#define HW1_SK_', text, re.M)) != len(KEYS) + 1 or \
                not re.search(r'^#define HW1_SK_Q\(key\) "\\"" key "\\":"$', text, re.M):
            failures.append(f'{OWNER} must define HW1_SK_Q(key) as "\\"" key "\\":" and nothing else')
        if '#include' in text:
            failures.append(f'{OWNER} must stay dependency-free')

    for name, marker, uses, _old in SENDERS:
        source = (component / name).read_text()
        block = strip_comments(extract_block(source, marker))
        if INCLUDE not in source:
            failures.append(f'{name} does not include {OWNER}')
        for key, count in uses.items():
            spelled = '\\"' + KEYS[key] + '\\"'
            if spelled in block:
                failures.append(f'{name} {marker.split("(")[0].split()[-1]}: spells {spelled}; '
                                f'use HW1_SK_Q(HW1_SK_{key})')
            got = len(re.findall(r'HW1_SK_Q\(HW1_SK_' + key + r'\)', block))
            if got != count:
                failures.append(f'{name}: expected {count} uses of HW1_SK_Q(HW1_SK_{key}), got {got}')

    for name, marker, old in RECEIVERS:
        source = (component / name).read_text()
        block = strip_comments(extract_block(source, marker))
        if INCLUDE not in source:
            failures.append(f'{name} does not include {OWNER}')
        subscripts = SUBSCRIPT.findall(block)
        spelled = [s for s in subscripts if s.startswith('"')]
        if spelled:
            failures.append(f'{name} {marker.split("(")[0].split()[-1]}: reads {", ".join(spelled)} '
                            f'by its own spelling; use the HW1_SK_* names')
        expected = ['HW1_SK_' + next(k for k, v in KEYS.items() if v == key) for key in old]
        if subscripts != expected:
            failures.append(f'{name}: indexes {subscripts}, expected {expected}')

    for path in sources(component):
        if path.name == OWNER:
            continue
        text = strip_comments(path.read_text(errors='replace'))
        for key in DISTINCTIVE:
            if '\\"' + key + '\\"' in text or re.search(r'\[\s*"' + key + r'"\s*\]', text):
                failures.append(f'{path.relative_to(component)} spells the sensor key "{key}"; '
                                f'use HW1_SK_{key.upper()} ({OWNER})')
    return failures


def c_literal(data: bytes) -> str:
    out = ''
    for b in data:
        ch = chr(b)
        out += '\\' + ch if ch in '"\\' else (ch if 32 <= b < 127 else f'\\{b:03o}')
    return '"' + out + '"'


def samples(fmt: bytes) -> list[str]:
    values = []
    for i, m in enumerate(CONVERSION.finditer(fmt)):
        length, conv = (m.group(1) or b'').decode(), m.group(2).decode()
        if conv == '%':
            continue
        if conv in 'di':
            values.append({'': f'(int){-1234 + 97 * i}', 'l': f'(long){-56789 + i}'}[length])
        elif conv in 'ouxX':
            values.append({'': f'(unsigned){4000000000 - i}u', 'l': f'(unsigned long){4294967200 + i}ul'}[length])
        elif conv in 'fFeEgG':
            values.append(f'(double){-123.4567891 * (i + 1)}')
        elif conv == 's':
            values.append(f'"s{i}"')
        else:
            raise ValueError(f'unexpected conversion {m.group(0)!r}')
    return values


def values(component: Path, cxx: str) -> list[str]:
    failures: list[str] = []
    lines, labels = [], []
    for name, marker, _uses, old in SENDERS:
        found = formats(extract_block((component / name).read_text(), marker))
        if len(found) != len(old):
            failures.append(f'{name}: {len(found)} snprintf formats, expected {len(old)}')
            continue
        for index, (expr, before) in enumerate(zip(found, old)):
            args = ''.join(', ' + v for v in samples(before))
            lines.append(f'  put({expr});\n  run({expr}{args});\n  run({c_literal(before)}{args});\n')
            labels.append((f'{name}#{index}', before))
    keys = []
    for name, marker, old in RECEIVERS:
        block = strip_comments(extract_block((component / name).read_text(), marker))
        found = SUBSCRIPT.findall(block)
        if len(found) != len(old):
            failures.append(f'{name}: {len(found)} key reads, expected {len(old)}')
            continue
        lines += [f'  put({expr});\n' for expr in found]
        keys += [(f'{name}[{i}]', key.encode()) for i, key in enumerate(old)]
    if failures:
        return failures

    include = INCLUDE + '\n' if (component / OWNER).exists() else ''
    code = f'''{include}#include <cstdarg>
#include <cstdio>
#include <cstring>
static void put(const char* s) {{ std::fwrite(s, 1, std::strlen(s) + 1, stdout); }}
__attribute__((format(printf, 1, 2))) static void run(const char* fmt, ...) {{
  char out[512];
  va_list ap; va_start(ap, fmt); std::vsnprintf(out, sizeof(out), fmt, ap); va_end(ap);
  put(out);
}}
int main() {{
{''.join(lines)}  return 0;
}}
'''
    with tempfile.TemporaryDirectory(prefix='hw1-sensor-json-keys-') as tmp:
        src, exe = Path(tmp) / 'keys.cpp', Path(tmp) / 'keys'
        src.write_text(code)
        subprocess.run([cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wformat=2',
                        '-I', str(component), str(src), '-o', str(exe)], check=True)
        out = subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE).stdout.split(b'\0')[:-1]
    expected_count = 3 * len(labels) + len(keys)
    if len(out) != expected_count:
        return [f'expected {expected_count} values, got {len(out)}']
    for i, (label, before) in enumerate(labels):
        fmt, new_text, old_text = out[3 * i:3 * i + 3]
        if fmt != before:
            failures.append(f'{label} format changed: {fmt!r} != {before!r}')
        if new_text != old_text:
            failures.append(f'{label} output changed: {new_text!r} != {old_text!r}')
    for (label, before), got in zip(keys, out[3 * len(labels):]):
        if got != before:
            failures.append(f'{label} reads {got!r}, the old code read {before!r}')
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=HERE.parents[3], help='repository root to read')
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    args = parser.parse_args()
    component = args.root.resolve() / 'components' / 'hardwareone'
    failures = contract(component) + values(component, args.cxx)
    if failures:
        for failure in failures:
            print(f'sensor_json_keys: {failure}', file=sys.stderr)
        return 1
    print(f'sensor_json_keys: {len(KEYS)} inter-device sensor JSON keys have one owner; '
          f'{sum(len(s[3]) for s in SENDERS)} sender formats and '
          f'{sum(len(r[2]) for r in RECEIVERS)} receiver reads unchanged')
    return 0


if __name__ == '__main__':
    sys.exit(main())
