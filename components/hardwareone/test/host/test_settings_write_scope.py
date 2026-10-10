#!/usr/bin/env python3
"""Run the production synchronous settings scope and persistence adapter.

The complete batch adapter and public scope class are extracted intact. The
production fixed-allocation batch core is included directly; task/TLS, locks,
clock, command identity and physical writers are controlled platform boundaries.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    sanitizers = parser.add_mutually_exclusive_group()
    sanitizers.add_argument('--sanitize', action='store_true')
    sanitizers.add_argument('--thread-sanitize', action='store_true')
    args = parser.parse_args()
    header = (COMPONENT / 'System_Settings.h').read_text()
    source = (COMPONENT / 'System_Settings.cpp').read_text()
    start = source.index('namespace {', source.index('// Settings batching is owner-scoped.'))
    end = source.index('// Filesystem locking', start)
    harness = (HERE / 'settings_write_scope_harness.cpp').read_text()
    harness = harness.replace('// INSERT_SCOPE_CLASS', extract_block(header, 'class SettingsWriteScope final') + ';')
    harness = harness.replace('// INSERT_ADAPTER', source[start:end])
    with tempfile.TemporaryDirectory(prefix='hw1-settings-write-scope-') as tmp:
        unit = Path(tmp) / 'scope.cpp'
        unit.write_text(harness)
        for slots in (3, 4):
            exe = Path(tmp) / f'scope-{slots}'
            command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-Wno-unused-const-variable',
                       '-Wno-unused-private-field',
                       '-pedantic', '-pthread', f'-DconfigNUM_THREAD_LOCAL_STORAGE_POINTERS={slots}',
                       '-I', str(HERE / 'web_batch_stubs'), '-I', str(COMPONENT),
                       str(unit), '-o', str(exe)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer', '-g']
            if args.thread_sanitize:
                command += ['-fsanitize=thread', '-fno-omit-frame-pointer', '-g']
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
