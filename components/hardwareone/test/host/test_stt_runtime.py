#!/usr/bin/env python3
"""Compile the production STT broker against deterministic HAL/session/task fakes.

The one-shot path tested here does not use the continuous-session latency
switches (System_STT.cpp, HW1_STT_*); they are compiled off, so this unit also
checks that every switch's off path still builds. test_stt_continuous.py runs
the continuous broker with all of them on (the defaults).
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
SWITCHES_OFF = [f'-D{name}=0' for name in (
    'HW1_STT_CAPTURE_FEATURES', 'HW1_STT_TRIM_SILENCE', 'HW1_STT_SPECULATIVE_FINAL',
    'HW1_STT_SHORT_CLOSE', 'HW1_STT_PUBLISH_WAKE', 'HW1_STT_LONG_PAUSE_FEED')]


def no_includes(text):
    return re.sub(r'^#include.*\n', '', text, flags=re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    backend = no_includes((SOURCE / 'System_Transcript.h').read_text() + '\n' + (SOURCE / 'System_STTLocal.h').read_text()).replace('#pragma once', '')
    interface = no_includes((SOURCE / 'System_STT.h').read_text()).replace('#pragma once', '')
    broker = no_includes((SOURCE / 'System_STT.cpp').read_text().split('// CLI_ADAPTER_BEGIN')[0])
    broker += '\n#endif\n'
    segmenter = no_includes((SOURCE / 'Audio_VadPolicy.h').read_text()).replace('#pragma once', '') + '\n' + no_includes((SOURCE / 'stt/stt_segmenter.h').read_text()).replace('#pragma once', '')
    harness = (HERE / 'stt_runtime_harness.cpp').read_text()
    harness = harness.replace('// INSERT_INTERFACE', backend + '\n' + interface + '\n' + segmenter)
    harness = harness.replace('// INSERT_BROKER', broker)
    with tempfile.TemporaryDirectory(prefix='hw1-stt-runtime-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', *SWITCHES_OFF, str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
