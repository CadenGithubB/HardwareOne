#!/usr/bin/env python3
"""Exercise actual command-result fan-out and STT redaction with escaped text."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--source-dir', '--component', dest='source_dir', type=Path, default=SOURCE,
                        help='component directory to read HardwareOne.cpp and System_Utils.cpp from')
    args = parser.parse_args()
    main = (args.source_dir / 'HardwareOne.cpp').read_text()
    utils = (args.source_dir / 'System_Utils.cpp').read_text()
    harness = (HERE / 'stt_privacy_harness.cpp').read_text()
    for marker, code in {
        'REDACTOR': function(utils, 'String redactOutputForLog('),
        'BROADCAST': function(main, 'void broadcastOutput(const String& s, const CommandContext& ctx)'),
        'DELIVER': function(main, 'void deliverCommandResult('),
    }.items():
        harness = harness.replace('// INSERT_' + marker, code)
    with tempfile.TemporaryDirectory(prefix='hw1-stt-privacy-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-sign-compare', str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        run = subprocess.run([str(exe)])
    if run.returncode:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
