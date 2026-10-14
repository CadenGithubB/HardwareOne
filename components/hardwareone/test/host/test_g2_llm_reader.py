#!/usr/bin/env python3
"""Compile and exercise the production G2 reply reader with bounded caller buffers."""
import argparse
from pathlib import Path
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    header = (COMPONENT / 'System_G2_Protocol.h').read_text()
    header = header.replace('#include "System_BuildConfig.h"', '')
    header = header.replace('#include <Arduino.h>', '')
    protocol = (COMPONENT / 'System_G2_Protocol.cpp').read_text()
    protocol = protocol.replace('#include "System_G2_Protocol.h"', '')
    protocol = protocol.replace('#include <Arduino.h>', '')
    owner = (COMPONENT / 'G2_Glasses.cpp').read_text()
    fragmenter = extract_block(owner, 'static bool sendPbFragmented(G2Temple& arm,')
    platform = ('#define ENABLE_BLUETOOTH 1\n#define ENABLE_G2_GLASSES 1\n'
                '#include <stdint.h>\nuint32_t millis() { return 0; }\n'
                'using portMUX_TYPE = int;\n'
                '#define portMUX_INITIALIZER_UNLOCKED 0\n'
                'void portENTER_CRITICAL(int*) {}\nvoid portEXIT_CRITICAL(int*) {}\n')
    with tempfile.TemporaryDirectory(prefix='hw1-llm-reader-') as directory:
        exe = Path(directory) / 'test'
        unit = Path(directory) / 'test.cpp'
        unit.write_text(platform + header + '\n' + protocol + '\n' +
                        '#include "g2_llm_reader_transport_stubs.h"\n' + fragmenter + '\n' +
                        (HERE / 'g2_llm_reader_harness.cpp').read_text())
        command = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-const-variable', '-I', str(COMPONENT), '-I', str(HERE), str(unit),
                   '-o', str(exe)]
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
