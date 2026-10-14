#!/usr/bin/env python3
"""Compile the production HTTP worker against deterministic task/network fakes."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def stripped(path):
    return re.sub(r'^#include.*\n', '', path.read_text(), flags=re.M).replace('#pragma once', '')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    source = here.parents[1]
    harness = (here / 'llm_http_runtime_harness.cpp').read_text()
    harness = harness.replace('// INSERT_INTERFACE', stripped(source / 'System_LLMHttp.h'))
    harness = harness.replace('// INSERT_IMPLEMENTATION', stripped(source / 'System_LLMHttp.cpp') +
                              '\n' + stripped(source / 'System_LLMHttpTerminal.inc'))
    with tempfile.TemporaryDirectory(prefix='hw1-llm-http-runtime-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-I', str(source), '-I', str(source.parent / 'hardwareone_libs/ArduinoJson/src'),
               str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
