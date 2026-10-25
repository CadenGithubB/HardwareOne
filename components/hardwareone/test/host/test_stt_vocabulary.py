#!/usr/bin/env python3
"""Run the actual System_STTVocabulary.cpp over guarded-VFS, lock and allocator fakes.

The module source is compiled unchanged except for removing its includes, with
the real stt/stt_transcript.cpp. Covers card-first loading, the internal copy,
whole-file validation (an invalid file or edit keeps the active list), staged
replacement through the shared writer (previous file kept on every failure),
generation and hotword snapshots for the decoder, and bounded display formatting.
It does not exercise SD/LittleFS, PSRAM or a real FreeRTOS mutex.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true', help='AddressSanitizer and UBSan')
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    module = re.sub(r'^#include.*\n', '', (SOURCE / 'System_STTVocabulary.cpp').read_text(), flags=re.M)
    header = re.sub(r'^#include.*\n|^#pragma once\n', '', (SOURCE / 'System_STTVocabulary.h').read_text(), flags=re.M)
    harness = (HERE / 'stt_vocabulary_harness.cpp').read_text()
    assert harness.count('// INSERT_MODULE') == 1
    harness = harness.replace('// INSERT_MODULE', header + module)
    with tempfile.TemporaryDirectory(prefix='hw1-stt-vocab-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        unit.write_text(harness)
        exe = Path(tmp) / 'test'
        cmd = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               f'-I{SOURCE}', str(unit), str(SOURCE / 'stt' / 'stt_transcript.cpp'), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=180)
        subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == '__main__':
    main()
