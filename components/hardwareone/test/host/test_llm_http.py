#!/usr/bin/env python3
"""Compile and exercise the production HTTP/SSE parser, without ESP hardware."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    components = here.parents[2]
    with tempfile.TemporaryDirectory(prefix='hw1-llm-http-') as tmp:
        exe = Path(tmp) / 'llm_http_tests'
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-I', str(components / 'hardwareone_libs/ArduinoJson/src'),
               str(here / 'test_llm_http.cpp'), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
