#!/usr/bin/env python3
"""Exercise the production workaround for ESP-SR 2.5.5 grammar replacement leaks."""
import argparse
from pathlib import Path
import subprocess
import struct
import tempfile
from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--model-pack', type=Path, help='Also check the actual packed default grammar')
    args = parser.parse_args()
    if args.model_pack:
        data = args.model_pack.read_bytes()
        cursor = 4
        demo = None
        for _ in range(struct.unpack_from('<I', data)[0]):
            model = data[cursor:cursor + 32].split(b'\0', 1)[0]
            files = struct.unpack_from('<I', data, cursor + 32)[0]
            cursor += 36
            for _ in range(files):
                name = data[cursor:cursor + 32].split(b'\0', 1)[0]
                offset, length = struct.unpack_from('<II', data, cursor + 32)
                if model == b'fst' and name == b'commands_en.txt':
                    demo = data[offset:offset + length]
                cursor += 40
        if demo is None:
            raise ValueError('Packed fst/commands_en.txt is missing')
        print(f'Actual packed command descriptor is present ({len(demo)} bytes); runtime overrides it during create')
    source = (HERE.parents[1] / 'System_ESPSR.cpp').read_text()
    harness = (HERE / 'espsr_grammar_lifetime_harness.cpp').read_text()
    parts = []
    for signature in ['static bool mnTableUpdateFailed(', 'struct MNCommandSnapshot', 'static bool mnUsesCommandFileGrammar()', 'static srmodel_data_t* findMNCommandFile(', 'static model_iface_data_t* createMNWithCommandFile(', 'static bool mnUpdateLocked()', 'static bool deinitMultiNet()']:
        parts.append(function(source, signature) + (';' if signature.startswith('struct ') else ''))
    harness = harness.replace('// INSERT_FUNCTIONS', '\n\n'.join(parts))
    with tempfile.TemporaryDirectory(prefix='hw1-sr-grammar-lifetime-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        binary = Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(binary)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
