#!/usr/bin/env python3
"""Exercise the complete production G2 codec's settings reads and write ACKs.

Only platform includes/locks are shimmed. No BLE, serial, or firmware build is
used; temporary host executables run the existing captured golden vectors too.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit('a host C++17 compiler is required')

    header = (COMPONENT / 'System_G2_Protocol.h').read_text()
    header = header.replace('#include "System_BuildConfig.h"', '')
    header = header.replace('#include <Arduino.h>', '')
    protocol = (COMPONENT / 'System_G2_Protocol.cpp').read_text()
    protocol = protocol.replace('#include "System_G2_Protocol.h"', '')
    protocol = protocol.replace('#include <Arduino.h>', '')
    platform = ('#define ENABLE_BLUETOOTH 1\n#define ENABLE_G2_GLASSES 1\n'
                '#include <stdint.h>\nuint32_t millis() { return 0; }\n'
                'using portMUX_TYPE = int;\n'
                '#define portMUX_INITIALIZER_UNLOCKED 0\n'
                'void portENTER_CRITICAL(int*) {}\nvoid portEXIT_CRITICAL(int*) {}\n')
    harness = (HERE / 'g2_settings_protocol_harness.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='hw1-g2-settings-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        executable = Path(tmp) / 'test'
        unit.write_text(platform + header + '\n' + protocol + '\n' + harness)
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-pedantic', '-Wno-unused-const-variable', '-I', str(COMPONENT),
                   str(unit), '-o', str(executable)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == '__main__':
    main()
