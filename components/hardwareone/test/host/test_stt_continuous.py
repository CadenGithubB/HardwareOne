#!/usr/bin/env python3
"""Run the actual continuous STT broker with concurrent tasks and controlled HAL/model fakes.

This tests ownership, buffering, sample accounting and delivery, not model accuracy,
ESP scheduling, DMA, or physical real-time throughput. No board or recordings used.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]


def no_includes(text):
    return re.sub(r'^#include.*\n', '', text, flags=re.M).replace('#pragma once', '')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group()
    group.add_argument('--sanitize', action='store_true', help='AddressSanitizer and UBSan')
    group.add_argument('--thread-sanitize', action='store_true', help='ThreadSanitizer (supported hosts only)')
    parser.add_argument('--repeat', type=int, default=1)
    args = parser.parse_args()
    if not 1 <= args.repeat <= 100:
        parser.error('--repeat must be 1..100')
    interface = '\n'.join(no_includes((SOURCE / name).read_text()) for name in
                          ('System_Transcript.h', 'System_STTLocal.h', 'System_STT.h', 'Audio_VadPolicy.h', 'stt/stt_segmenter.h'))
    broker = no_includes((SOURCE / 'System_STT.cpp').read_text().split('// CLI_ADAPTER_BEGIN')[0]) + '\n#endif\n'
    harness = (HERE / 'stt_continuous_harness.cpp').read_text()
    harness = harness.replace('// INSERT_INTERFACE', interface).replace('// INSERT_BROKER', broker)
    with tempfile.TemporaryDirectory(prefix='hw1-stt-continuous-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = ['clang++', '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        elif args.thread_sanitize:
            cmd[1:1] = ['-fsanitize=thread', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        for _ in range(args.repeat):
            subprocess.run([str(exe)], check=True, timeout=90)


if __name__ == '__main__':
    main()
