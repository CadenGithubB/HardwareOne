#!/usr/bin/env python3
"""Compile the production validator against the pinned actual software codec."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--jpeg-component', type=Path, required=True)
    p.add_argument('--private-bad', type=Path)
    p.add_argument('--sanitize', action='store_true')
    args = p.parse_args()
    jpeg = args.jpeg_component.resolve()
    flags = ['-Wall', '-Wextra']
    if args.sanitize: flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='jpeg-validation-') as tmp:
        tmp = Path(tmp)
        for fast in (0, 1, 2):
            for scale in (0, 1):
                config = tmp / f'config-{fast}-{scale}'
                config.mkdir()
                (config / 'sdkconfig.h').write_text(f'''#pragma once
#define CONFIG_JD_USE_ROM 0
#define CONFIG_JD_SZBUF 512
#define CONFIG_JD_FORMAT 0
#define CONFIG_JD_USE_SCALE {scale}
#define CONFIG_JD_TBLCLIP 1
#define CONFIG_JD_FASTDECODE {fast}
#define CONFIG_JD_DEFAULT_HUFFMAN 1
''')
                includes = ['-I', str(config), '-I', str(jpeg/'tjpgd'), '-I', str(jpeg/'include'),
                            '-I', str(HERE/'jpeg_stubs'), '-I', str(ROOT)]
                objects = []
                for i, source in enumerate((jpeg/'tjpgd/tjpgd.c', jpeg/'jpeg_default_huffman_table.c')):
                    obj = tmp / f'{fast}-{scale}-{i}.o'
                    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', *flags, *includes,
                                    '-c', str(source), '-o', str(obj)], check=True)
                    objects.append(str(obj))
                binary = tmp / f'validate-{fast}-{scale}'
                subprocess.run([os.environ.get('CXX','c++'), '-std=c++17', *flags, *includes,
                                str(HERE/'test_jpeg_validation.cpp'), str(ROOT/'HAL_JPEG.cpp'),
                                str(ROOT/'HAL_JPEG_Software.cpp'), *objects, '-o', str(binary)], check=True)
                command = [str(binary), str(HERE/'jpeg_fixtures')]
                if args.private_bad: command.append(str(args.private_bad.resolve()))
                subprocess.run(command, check=True)

if __name__ == '__main__': main()
