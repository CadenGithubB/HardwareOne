#!/usr/bin/env python3
"""Compile production chat/backend lifecycle with mocked transport boundaries.

The complete production implementations and headers are read on every run; only
platform includes are removed. HTTP/UART I/O, settings and allocation are mocks.
This tests dispatch and conversation ownership, not ESP task races or sockets.
"""
from __future__ import annotations
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / 'hardwareone_libs/ArduinoJson/src'


def without_includes(path):
    return re.sub(r'^\s*#\s*include[^\n]*$', '', path.read_text(), flags=re.M)


def flag_header(text, flags):
    for name, value in flags.items():
        text, count = re.subn(r'(?m)^(\s*#define\s+' + re.escape(name) + r'\s+)\d+\b',
                              lambda m: m[1] + str(value), text)
        if count != 1:
            raise AssertionError(f'expected one build literal for {name}, found {count}')
    return text


def build_gate_matrix(cxx, temporary):
    original = (COMPONENT / 'System_BuildConfig.h').read_text()
    base = {'ENABLE_LLM_BACKEND': 1, 'ENABLE_LLM_SOURCE_ONBOARD': 0,
            'ENABLE_LLM_SOURCE_CM5': 0, 'ENABLE_LLM_SOURCE_HTTP': 1}
    cases = [
        ('http-only', {}, True),
        ('mixed', {'ENABLE_LLM_SOURCE_ONBOARD': 1, 'ENABLE_LLM_SOURCE_CM5': 1}, True),
        ('http-disabled', {'ENABLE_LLM_SOURCE_HTTP': 0, 'ENABLE_LLM_SOURCE_CM5': 1}, True),
        ('feature-disabled', {'ENABLE_LLM_BACKEND': 0, 'ENABLE_LLM_SOURCE_HTTP': 0}, True),
        ('missing-source', {'ENABLE_LLM_SOURCE_HTTP': 0}, False),
        ('source-without-feature', {'ENABLE_LLM_BACKEND': 0}, False),
        ('http-without-wifi', {'NETWORK_FEATURE_LEVEL': 0}, False),
    ]
    for name, overrides, expected in cases:
        header = temporary / (name + '.h')
        header.write_text(flag_header(original, {**base, **overrides}))
        result = subprocess.run([cxx, '-E', '-x', 'c++', '-DARDUINO_XIAO_ESP32S3_DEV=1',
                                 str(header)], text=True, capture_output=True)
        if (result.returncode == 0) != expected:
            raise AssertionError(f'build gate {name}: expected success={expected}\n{result.stderr}')
        print('build gate:', name, 'accepted' if expected else 'rejected')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit('C++17 compiler required')
    harness = (HERE / 'llm_http_integration_harness.cpp').read_text()
    headers = '\n'.join(without_includes(COMPONENT / name) for name in (
        'System_LLMTypes.h', 'System_LLMHomeMenuCore.h', 'System_LLMBackend.h',
        'System_LLMReplyLimits.h', 'System_LLMChat.h'))
    source = '\n'.join(without_includes(COMPONENT / name) for name in (
        'System_LLMBackend.cpp', 'System_LLMChat.cpp'))
    unit = harness.replace('// INSERT_PRODUCTION_HEADERS', headers).replace(
        '// INSERT_PRODUCTION_IMPLEMENTATIONS', source)
    with tempfile.TemporaryDirectory(prefix='hw1-llm-integration-') as directory:
        temporary = Path(directory)
        build_gate_matrix(args.cxx, temporary)
        cpp = temporary / 'integration.cpp'
        cpp.write_text(unit)
        for name, cm5, http in [('http-only', 0, 1), ('mixed-remote', 1, 1), ('http-disabled', 1, 0)]:
            binary = temporary / name
            command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                       '-Wno-unused-parameter', '-Wno-unused-function',
                       '-DENABLE_LLM_BACKEND=1', '-DENABLE_LLM_SOURCE_ONBOARD=0',
                       f'-DENABLE_LLM_SOURCE_CM5={cm5}', f'-DENABLE_LLM_SOURCE_HTTP={http}',
                       '-I', str(JSON_INCLUDE), '-I', str(HERE / 'web_batch_stubs'),
                       str(cpp), '-o', str(binary)]
            if args.sanitize:
                command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)
            print('production lifecycle:', name, 'passed')

if __name__ == '__main__':
    main()
