#!/usr/bin/env python3
"""Run this device's ESP-NOW message numbers against the production receive
duplicate ring (ESP-NOW 1.0, E21).

Compiled: System_ESPNow_MsgIdCore.h, the production generateMessageId() and the
production accept memory (v4_accept_find / v4_accept_lookup / v4_accept_record,
1.0 step 5b) with its entry type and TTL from System_ESPNow.cpp, over a fake
clock (espnow_msgid_dedup_harness.cpp). Checks:
the seed is never 0; the counter goes FFFFFFFF -> 1 and never hands out 0; 300
frames from a random start across the wrap are all new to the receiver and each
re-delivery is caught; fragments of one msgId stay distinct; a sender restarting
within the TTL from a new random start loses no frame (the old fixed start of 1
lost its first 20, kept as a control); expiry after the TTL is unchanged. The
accept memory: an accepted frame is remembered as accepted and a silent one as
silent; a refused frame leaves no entry, so its retry is processed and then
remembered; the class and the fragment are part of the key; an expired entry is
forgotten and refreshed in place with the new verdict; all 21 fragments of a
completed message stay remembered; the entry is still 20 bytes.

Source: the state's counter is seeded with espnowmsgid::seed(esp_random()) where
ESP-NOW builds its state, and generateMessageId() goes through espnowmsgid::take.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(a.component / 'test/host'))
    from test_web_batch_handlers import extract_block

    source = (a.component / 'System_ESPNow.cpp').read_text()
    core = (a.component / 'System_ESPNow_MsgIdCore.h').read_text().replace('#pragma once\n', '')

    failures = []
    if 'espnowmsgid::seed(esp_random())' not in source:
        failures.append('the msgId counter is not seeded with espnowmsgid::seed(esp_random())')
    generate = extract_block(source, 'uint32_t generateMessageId() {')
    if 'espnowmsgid::take(&gEspNow->nextMessageId)' not in generate:
        failures.append('generateMessageId() does not go through espnowmsgid::take')
    if failures:
        for f in failures:
            print('FAIL:', f)
        return 1

    types = '\n'.join(m.group(0) for m in (
        re.search(r'^enum class V4DedupClass[^\n]*\n', source, re.M),
        re.search(r'^enum class V4AcceptMemo[^\n]*\n', source, re.M),
        re.search(r'^struct V4DedupEntry \{.*?\n\};\n', source, re.M | re.S)))
    ttl = re.search(r'^static constexpr uint32_t V4_DEDUP_TTL_MS = \d+;\n', source, re.M).group(0)
    ring = '\n\n'.join((extract_block(source, 'struct V4AcceptKey') + ';',
                         extract_block(source, 'static V4DedupEntry* v4_accept_find('),
                         extract_block(source, 'static V4AcceptMemo v4_accept_lookup('),
                         extract_block(source, 'static void v4_accept_record(')))

    harness = (HERE / 'espnow_msgid_dedup_harness.cpp').read_text()
    for tag, value in [('// INSERT_MSGID_CORE', core), ('// INSERT_GENERATE', generate),
                       ('// INSERT_DEDUP_TYPES', types), ('// INSERT_DEDUP_TTL', ttl),
                       ('// INSERT_DEDUP_RING', ring)]:
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-msgid-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        return subprocess.run([str(exe)], timeout=60).returncode


if __name__ == '__main__':
    sys.exit(main())
