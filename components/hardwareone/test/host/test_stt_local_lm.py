#!/usr/bin/env python3
"""Run the actual System_STTLocal.cpp language-model loading and fallback policy.

The P4 backend source is compiled unchanged except for removing its includes,
with ENABLE_STT_LM on and off, the shared strength and an explicit 0.75 override,
against VFS/heap/log/runtime fakes and the real
stt/stt_lm.cpp. Covers load-once caching, one-shot lifetime, custom words,
invalid/missing files, memory refusal, release under pressure, cancellation and
log-once behaviour. It does not exercise SD/LittleFS, PSRAM or ESP-DL.
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
    # Keep the host caller's configured strength in sync with the shared default;
    # production includes are removed below to substitute host-only dependencies.
    config = (SOURCE / 'System_BuildConfig.h').read_text()
    strength = re.search(r'^\s*#\s*define\s+HW1_STT_LM_STRENGTH\s+(\S+)', config, re.M)
    assert strength, 'Shared LM-strength default is missing'
    backend = re.sub(r'^#include.*\n', '', (SOURCE / 'System_STTLocal.cpp').read_text(), flags=re.M)
    harness = (HERE / 'stt_local_lm_harness.cpp').read_text()
    assert harness.count('// INSERT_BACKEND') == 1
    harness = harness.replace('// INSERT_BACKEND', backend)
    with tempfile.TemporaryDirectory(prefix='hw1-stt-local-lm-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        unit.write_text(harness)
        variants = ((1, 'configured', strength.group(1)), (1, 'override', '0.75f'),
                    (0, 'disabled', strength.group(1)))
        for enabled, name, configured_strength in variants:
            exe = Path(tmp) / f'test-{name}'
            cmd = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                   '-DENABLE_LOCAL_STT=1', f'-DENABLE_STT_LM={enabled}', '-DCONFIG_IDF_TARGET_ESP32P4=1',
                   f'-DHW1_STT_LM_STRENGTH={configured_strength}',
                   f'-I{SOURCE}', f'-I{HERE}', str(unit), str(SOURCE / 'stt' / 'stt_lm.cpp'), '-o', str(exe)]
            if args.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(cmd, check=True, timeout=180)
            subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == '__main__':
    main()
