#!/usr/bin/env python3
"""Compile the actual recorder VAD block and compare its pre-extraction policy."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (SOURCE / 'System_Microphone.cpp').read_text()
    block = extract_block(source, 'if (gRecSilenceStopMs > 0)')
    assert 'adaptiveVadDecision(' in block
    names = ('kRecFloorWinChunks', 'kRecSpeechFloorAvg', 'kRecSilenceFloorAvg',
             'kRecVadMinMs', 'kRecPreLatchHoldMaxMs')
    constants = []
    for name in names:
        match = re.search(r'static const (?:size_t|int32_t|uint32_t)\s+' + name + r'\s*=.*?;', source)
        assert match, name
        constants.append(match.group(0))
    harness = (HERE / 'audio_vad_policy_harness.cpp').read_text()
    harness = harness.replace('// INSERT_CONSTANTS', '\n'.join(constants))
    harness = harness.replace('// INSERT_RECORDER_BLOCK', block)
    with tempfile.TemporaryDirectory(prefix='hw1-audio-vad-') as temporary:
        unit = Path(temporary) / 'vad.cpp'
        binary = Path(temporary) / 'vad'
        unit.write_text(harness)
        command = ['clang++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                   str(unit), '-I' + str(SOURCE), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)

if __name__ == '__main__':
    main()
