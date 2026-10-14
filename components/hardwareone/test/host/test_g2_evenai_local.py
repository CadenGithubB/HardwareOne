#!/usr/bin/env python3
"""Run the shipping native voice adapter/packet guard with mocked I/O boundaries."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    source = (COMPONENT / 'G2_Glasses.cpp').read_text()
    native = source[source.index('struct G2EvenAiSessionState'):]
    session = extract_block(native, 'struct G2EvenAiSessionState') + ';\n'
    ready = extract_block(source, 'static bool g2TempleReadyAtGeneration(')
    bound_arm = extract_block(native, 'static G2Temple* g2EvenAiBoundArm(')
    guard = extract_block(native, 'static bool g2EvenAiGuardMatches(')
    selected = extract_block(source, 'static bool g2EvenAiUseLocalSpeech(')
    adapter = source[source.index('// Local native voice adapter.'):source.index('// heartbeatWorkerTask (never the BLE notify task).')]
    tick = extract_block(native, 'static void g2EvenAiSessionTick()')
    timing = '\n'.join(re.findall(r'static const uint32_t\s+kEvenAi(?:HbInterval|SessionMax)Ms\s*=\s*\d+;', native))
    assert len(timing.splitlines()) == 2
    harness = (HERE / 'g2_evenai_local_harness.cpp').read_text()
    # Exercise the shipping codec at every mocked native send boundary. Only
    # target configuration/platform includes are replaced; no builders are
    # reconstructed in the fixture.
    header = (COMPONENT / 'System_G2_Protocol.h').read_text()
    header = header.replace('#include "System_BuildConfig.h"', '')
    header = header.replace('#include <Arduino.h>', '')
    protocol = (COMPONENT / 'System_G2_Protocol.cpp').read_text()
    protocol = protocol.replace('#include "System_G2_Protocol.h"', '')
    protocol = protocol.replace('#include <Arduino.h>', '')
    assert harness.count('// INSERT_PROTOCOL') == 1
    harness = harness.replace('// INSERT_PROTOCOL', header + '\n' + protocol)
    harness = harness.replace('// INSERT_SESSION', session).replace('// INSERT_GUARD', guard + '\n' + selected)
    harness = harness.replace('// INSERT_READY', ready).replace('// INSERT_BOUND_ARM', bound_arm)
    harness = harness.replace('// INSERT_ADAPTER', adapter)
    harness = harness.replace('// INSERT_TICK', timing + '\n' + tick)
    # Compile both the selected path and the real disabled stubs. No local-STT
    # symbols may escape into XIAO/CM5 or a speech-only P4 build.
    with tempfile.TemporaryDirectory(prefix='hw1-evenai-') as directory:
        unit, exe = Path(directory) / 'test.cpp', Path(directory) / 'test'
        unit.write_text(harness)
        for stt, chat in ((1, 1), (0, 1), (1, 0)):
            cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-variable',
                   '-DENABLE_BLUETOOTH=1', '-DENABLE_G2_GLASSES=1',
                   '-DENABLE_MICROPHONE=0',  # isolate the independent legacy UART recorder
                   f'-DENABLE_LOCAL_STT={stt}', f'-DENABLE_LLM_BACKEND={chat}',
                   '-I', str(COMPONENT), '-I', str(HERE), str(unit), '-o', str(exe)]
            if args.sanitize:
                cmd += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(cmd, check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    main()
