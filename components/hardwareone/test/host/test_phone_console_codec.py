#!/usr/bin/env python3
"""Run the phone link v2 console records (BLE_PhonePipeConsoleCore.h) as shipped.

The harness pins the record header bytes (len little-endian) and runs the P4
ingress validator over every type, flag pattern and boundary length: COMMAND
1..511 and BINARY 13..489 with the OTA magic are delivered, the reserved RESUME
and unknown types are skipped (0..1024, no MORE), a P4 -> phone type, MORE
outside TEXT/BLOB, flag bits 1-7, a bad length or magic is malformed. It scans
long streams split anywhere (one byte at a time included) and checks that a
malformed record shows exactly when its offending byte arrives and never
earlier. The P4 egress chain machine is run at the 4608 B cap (4608 passes,
4609 and MORE at 4608 do not), with interleaving and ABORT rules; the phone
receiver mirrors it, and a random walk shows the two agree record by record.
This driver parses the INFO payloads the harness prints as JSON and checks the
key order and values. The text check keeps the header pure.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
INFO_KEYS = ['v', 'fw', 'model', 'line', 'bin', 'auth', 'resume']
STRING = r'"(?:[^"\\\x00-\x1f\x7f-\xff]|\\["\\]|\\u00[0-9a-f]{2})*"'
COMPACT = re.compile(r'\{"v":1,"fw":' + STRING + r',"model":' + STRING +
                     r',"line":511,"bin":489,"auth":true,"resume":false\}')


def check_sources(component: Path) -> None:
    header = (component / 'BLE_PhonePipeConsoleCore.h').read_text()
    includes = re.findall(r'^#include\s+(\S+)', header, re.MULTILINE)
    require(includes == ['<cstddef>', '<cstdint>', '<cstring>'],
            f'the record layer stays pure (no BLE, OS or Arduino): {includes}')
    code = re.sub(r'//[^\n]*', '', header)
    require(not re.search(r'\bnew\b|malloc|std::string|std::vector|snprintf|esp_|Arduino|xSemaphore', code),
            'the record layer allocates nothing and calls no SDK')
    for constant in ('kInfo = 0x01', 'kText = 0x02', 'kBlob = 0x03', 'kCard = 0x04', 'kMirror = 0x05',
                     'kAbort = 0x06', 'kTicket = 0x07', 'kCommand = 0x81', 'kBinary = 0x82', 'kResume = 0x83'):
        require(constant in header, f'record type {constant}')
    ota = (component / 'System_OTA.cpp').read_text()
    require("{0x00, 'H', 'W', '1', 'O', 'T', 'A', 0x01}" in ota and
            "kOtaMagic[8] = {0x00, 'H', 'W', '1', 'O', 'T', 'A', 0x01}" in header,
            'BINARY carries the OtaBleWire magic the OTA handler checks')
    head = re.search(r'kBleUploadHeaderSize = (\d+);', ota)
    chunk = re.search(r'kBleUploadChunkMax = (\d+);', ota)
    require(head and chunk and int(head.group(1)) == 12 and int(head.group(1)) + int(chunk.group(1)) == 489,
            'BINARY 13..489 is the OTA frame: 12 B head plus a 1..477 B chunk')
    limits = re.search(r'CMD_RESULT_MAX = (\d+);', (component / 'System_CommandLimits.h').read_text())
    require(limits and int(limits.group(1)) + 512 == 4608 and 'kChainMax = 4608' in header,
            'a TEXT chain holds a whole command result (CMD_RESULT_MAX plus headroom)')


def check_info(output: str) -> int:
    count = 0
    for line in output.splitlines():
        if not line.startswith('INFO '):
            continue
        raw = bytes.fromhex(line[5:])
        require(len(raw) <= 256 and raw.isascii(), f'INFO is ASCII and fits its record: {raw!r}')
        doc = json.loads(raw.decode('ascii'), object_pairs_hook=lambda pairs: pairs)
        require([key for key, _ in doc] == INFO_KEYS, f'INFO key order: {raw!r}')
        values = dict(doc)
        require(values['v'] == 1 and values['line'] == 511 and values['bin'] == 489, raw)
        require(values['auth'] is True and values['resume'] is False, raw)
        require(isinstance(values['fw'], str) and isinstance(values['model'], str) and values['model'], raw)
        require(COMPACT.fullmatch(raw.decode('ascii')) is not None, f'INFO is compact, in key order: {raw!r}')
        count += 1
    require(count >= 10, f'INFO payloads printed: {count}')
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone (mutation runs)')
    args = parser.parse_args()
    component = (args.component or COMPONENT).resolve()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    flags = ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g']
    check_sources(component)
    with tempfile.TemporaryDirectory(prefix='hw1-phone-console-codec-') as directory:
        binary = Path(directory) / 'codec'
        subprocess.run([args.cxx, *flags, '-I', str(component), str(HERE / 'phone_console_codec_harness.cpp'),
                        '-o', str(binary)], check=True, timeout=180)
        output = subprocess.check_output([str(binary)], text=True, timeout=300)
    infos = check_info(output)
    print(output.splitlines()[-1])
    print(f'Phone console records: header, ingress bounds, split streams, the 4608 B chain cap, egress and '
          f'receiver agreement and {infos} INFO payloads passed')


if __name__ == '__main__':
    main()
