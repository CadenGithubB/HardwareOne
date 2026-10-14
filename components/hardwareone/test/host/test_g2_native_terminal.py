#!/usr/bin/env python3
"""Exercise the shipping native Terminal owner against controlled service boundaries."""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (COMPONENT / 'G2_Glasses.cpp').read_text()
    adapter = source[source.index('// Native Terminal adapter.'):source.index('// End native Terminal adapter.')]
    header = (COMPONENT / 'System_G2_Protocol.h').read_text().replace('#include "System_BuildConfig.h"', '').replace('#include <Arduino.h>', '')
    protocol = (COMPONENT / 'System_G2_Protocol.cpp').read_text().replace('#include "System_G2_Protocol.h"', '').replace('#include <Arduino.h>', '')
    globals_ = source[source.index('// Native Terminal owns its own lifecycle,'):source.index('// =============================================================================\n// Constants')]
    harness = (HERE / 'g2_native_terminal_harness.cpp').read_text()
    harness = harness.replace('// INSERT_PROTOCOL', header + '\n' + protocol)
    harness = harness.replace('// INSERT_GLOBALS', globals_)
    harness = harness.replace('// INSERT_ADAPTER', adapter)
    with tempfile.TemporaryDirectory(prefix='hw1-native-terminal-') as directory:
        unit, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
        unit.write_text(harness)
        for stt, chat, http in ((1, 1, 1), (0, 1, 1), (1, 0, 1), (1, 1, 0)):
            command = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-Wno-unused-variable',
                       '-DENABLE_BLUETOOTH=1', '-DENABLE_G2_GLASSES=1',
                       f'-DENABLE_LOCAL_STT={stt}', f'-DENABLE_LLM_BACKEND={chat}', f'-DENABLE_LLM_SOURCE_HTTP={http}',
                       '-I', str(COMPONENT), '-I', str(COMPONENT.parent / 'hardwareone_libs/ArduinoJson/src'), str(unit), '-o', str(exe)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(command, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
