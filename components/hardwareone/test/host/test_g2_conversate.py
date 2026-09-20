#!/usr/bin/env python3
"""Compile the full shipping protocol and session policy; shim only platform includes."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    header = (COMPONENT / 'System_G2_Protocol.h').read_text()
    header = header.replace('#include "System_BuildConfig.h"', '')
    header = header.replace('#include <Arduino.h>', '')
    source = (COMPONENT / 'System_G2_Protocol.cpp').read_text()
    source = source.replace('#include "System_G2_Protocol.h"', '')
    source = source.replace('#include <Arduino.h>', '')
    harness = (HERE / 'g2_conversate_harness.cpp').read_text()
    integration = (HERE / 'g2_conversate_owner_harness.cpp').read_text()
    owner = (COMPONENT / 'G2_Glasses.cpp').read_text()
    # Definitions after the Native Conversate qualification marker have no
    # forward-declaration ambiguity. Hardware boundaries are mocked below.
    owner_impl = owner[owner.index('// ── Native Conversate microphone qualification'):]
    definitions = '\n'.join(extract_block(owner_impl, marker) for marker in (
        'static uint32_t g2ConversateNextMagic(', 'static bool g2ConversateBusy(',
        'static bool g2ConversateConnections(', 'static void g2ConversateStop(',
        'static bool g2ConversateDeclineEvenAi(',
        'static void g2ConversateOnRx(', 'static void g2ConversateTick('))
    definitions = (extract_block(owner, 'static inline uint32_t g2EvenAiNextMagic(') + '\n' + definitions +
                   '\n' + extract_block(owner, 'static const char* cmd_g2conversate('))
    wake = extract_block(owner[owner.index('// Runs on the control owner after a CRC-verified native WAKE'):],
                         'static void g2EvenAiOnWakeUp(')
    assert wake.index('if (g2ConversateDeclineEvenAi(temple, rxGeneration)) return;') < wake.index('g2ConversateStop(')
    assert wake.index('g2ConversateStop(') < wake.index('gEvenAiLastWakeMs =')
    integration = integration.replace('// INSERT_PRODUCTION_OWNER', definitions)
    rx_definitions = owner[owner.index('struct G2RxPacket {'):owner.index('static bool g2RxEvenAiCtrlStatus(')]
    rx_definitions += '\n'.join(extract_block(owner, marker) for marker in (
        'static void g2RingPeekCmdMagic(', 'static bool g2PeekNestedVarint(',
        'static bool g2RxEvenAiCtrlStatus(', 'static uint8_t g2RxConversateTerminal(',
        'static bool g2RxPacketEnqueue(', 'static bool g2RxPacketDequeue(', 'static void g2RxReset('))
    rx_definitions += extract_block(owner[owner.index('// Runs on the reused heartbeat/control owner'):],
                                    'static void processNotify(')
    integration = integration.replace('// INSERT_PRODUCTION_RX', rx_definitions)
    with tempfile.TemporaryDirectory(prefix='hw1-conversate-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        unit.write_text('#define ENABLE_BLUETOOTH 1\n#define ENABLE_G2_GLASSES 1\n'
                        '#include <stdint.h>\nuint32_t clockMs = 0;\n'
                        'uint32_t millis() { return clockMs; }\n'
                        'using portMUX_TYPE = int;\n'
                        '#define portMUX_INITIALIZER_UNLOCKED 0\n'
                        'void portENTER_CRITICAL(int*) {}\nvoid portEXIT_CRITICAL(int*) {}\n' +
                        header + '\n' + source + '\n' + integration + '\n' + harness)
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-Wno-unused-const-variable',
                   '-I', str(COMPONENT), str(unit), '-o', str(Path(tmp) / 'test')]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(Path(tmp) / 'test')], check=True)


if __name__ == '__main__':
    main()
