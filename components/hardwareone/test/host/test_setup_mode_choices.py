#!/usr/bin/env python3
"""Exercise the shipping first-time setup menu and parser across feature gates."""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    header = (COMPONENT / 'System_FirstTimeSetup.h').read_text()
    source = (COMPONENT / 'System_FirstTimeSetup.cpp').read_text()
    declarations = '\n'.join(extract_block(header, marker) + ';' for marker in
                             ('enum SetupMode ', 'struct SetupModeOption'))
    declarations += '\n' + extract_block(source, 'static const SetupModeOption kSetupModes') + ';'
    functions = '\n\n'.join(extract_block(source, marker) for marker in
                            ('size_t getSetupModeCount(', 'const SetupModeOption* getSetupModeOption(',
                             'bool parseSetupModeInput('))
    harness = (HERE / 'setup_mode_choices_harness.cpp').read_text()
    harness = harness.replace('// INSERT_PRODUCTION', declarations + '\n' + functions)
    with tempfile.TemporaryDirectory(prefix='hw1-setup-mode-choices-') as tmp:
        directory = Path(tmp)
        unit = directory / 'choices.cpp'
        unit.write_text(harness)
        for migration in (0, 1):
            for wifi in (0, 1):
                for http in (0, 1):
                    exe = directory / f'choices-{migration}-{wifi}-{http}'
                    command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                               f'-DENABLE_MIGRATION_TOOL={migration}', f'-DENABLE_WIFI={wifi}',
                               f'-DENABLE_HTTP_SERVER={http}', '-I', str(HERE / 'web_batch_stubs'),
                               str(unit), '-o', str(exe)]
                    if args.sanitize:
                        command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                                    '-fno-omit-frame-pointer', '-g']
                    subprocess.run(command, check=True, timeout=90)
                    subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    main()
