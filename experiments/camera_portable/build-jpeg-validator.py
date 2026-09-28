#!/usr/bin/env python3
"""Build the strict host decoder against a matching libjpeg installation.

Example: python3 build-jpeg-validator.py --jpeg-prefix /path/to/libjpeg
The prefix must contain include/jpeglib.h and lib/libjpeg. Outputs stay private.
Use --include-dir/--lib-dir to override either directory. Decode one image with
private/jpeg-validate image.jpg; exit zero requires no libjpeg warning or error.
"""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import subprocess

HERE = Path(__file__).resolve().parent

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jpeg-prefix', type=Path, required=True)
    parser.add_argument('--include-dir', type=Path)
    parser.add_argument('--lib-dir', type=Path)
    parser.add_argument('--output', type=Path, default=HERE / 'private' / 'jpeg-validate')
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('cc'))
    args = parser.parse_args()
    include = (args.include_dir or args.jpeg_prefix / 'include').resolve()
    library = (args.lib_dir or args.jpeg_prefix / 'lib').resolve()
    if not args.cc or not (include / 'jpeglib.h').is_file() or not library.is_dir():
        parser.error('compiler and matching libjpeg include/library directories are required')
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([*shlex.split(args.cc), '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(include), str(HERE / 'jpeg_validate.c'), '-L', str(library),
                    '-Wl,-rpath,' + str(library), '-ljpeg', '-o', str(output)], check=True)
    print(output)

if __name__ == '__main__':
    main()
