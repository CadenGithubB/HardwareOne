#!/usr/bin/env python3
"""Exercise production SR teardown, partial PCM feed and deferred voice authority."""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1] / 'System_ESPSR.cpp'


def function(source, signature):
    start = source.index(signature)
    pos = source.index('{', start)
    while ';' in source[start:pos]:  # Skip a matching forward declaration.
        start = source.index(signature, start + len(signature))
        pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        if source[end] == '{': depth += 1
        elif source[end] == '}': depth -= 1
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = SOURCE.read_text()
    harness = (HERE / 'espsr_runtime_harness.cpp').read_text()
    for marker, signatures in {
        'LIFECYCLE': ['static bool srWaitForWorker(', 'static bool stopESPSRLocked()'],
        'FEED': ['static void srFeedTask(void*)'],
        'DSP_SETTINGS': ['static void prepareSRAudioProcessing()'],
        'MODEL_SELECTION': ['static bool initSRModels()'],
        'MN_LOCK': ['static bool ensureMNCommandMutex()', 'static bool lockMN(',
                    'static void unlockMN()', 'static bool mnCommandsReady()'],
        'VOICE': ['static void voiceDisarmInternal()', 'static bool voiceArmFromContextInternal(',
                  'struct VoiceCommandJob', 'static void runVoiceCommandJob(',
                  'static bool executeVoiceCommandAsArmedUser('],
    }.items():
        chunks = []
        for signature in signatures:
            chunk = function(source, signature)
            chunks.append(chunk + (';' if signature.startswith('struct ') else ''))
        harness = harness.replace('// INSERT_' + marker, '\n\n'.join(chunks))
    with tempfile.TemporaryDirectory(prefix='hw1-sr-runtime-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
               '-Wno-unused-function', str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
