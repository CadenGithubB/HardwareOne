#!/usr/bin/env python3
"""Exercise production native settings transactions, command and ATT fencing.

Only RTOS, command/String and physical BLE boundaries are shimmed. The full
production codec and extracted owner functions run against synthetic frames.
No device, network, or credentials are accessed.
"""
import argparse
from pathlib import Path
import re
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
    owner = (COMPONENT / 'G2_Glasses.cpp').read_text()
    header = (COMPONENT / 'System_G2_Protocol.h').read_text().replace('#include "System_BuildConfig.h"', '').replace('#include <Arduino.h>', '')
    protocol = (COMPONENT / 'System_G2_Protocol.cpp').read_text().replace('#include "System_G2_Protocol.h"', '').replace('#include <Arduino.h>', '')
    functions = [extract_block(owner.replace('static bool g2BothFirmwareExact(const char* expected);', ''), name) for name in (
        'static G2LinkSnapshot g2LinkSnapshot(',
        'static bool g2TempleReadyAtGeneration(',
        'static bool g2TempleTransportAtGeneration(',
        'bool isG2Connected(',
        'static uint8_t g2NextSettingsMagic(',
        'static void g2SettingsEchoPublish(',
        'static bool g2SettingsEchoSnapshot(',
        'static void g2SettingsEchoInvalidateFreshness(',
        'static void g2SettingsAckArm(',
        'static void g2SettingsAckClear(',
        'static void g2SettingsAckPublish(',
        'static bool g2SettingsAckSnapshot(',
        'static bool g2SettingsReceive(',
        'static bool sendEnvelopeNoMutex(',
        'static bool sendEnvelopeExactGeneration(',
        'static bool g2GlassesEchoValue(',
        'static bool g2SettingsReadFresh(',
        'static void g2SettingsPrimeIfNeeded(',
        'static bool g2SettingsFirmwareKnown(',
        'static bool g2BothFirmwareExact(',
        'static const char* cmd_g2glasses(',
    )]
    state = owner[owner.index('static G2SettingsEcho gSettingsEcho;'):owner.index('enum class G2PolicyFeature')]
    knobs = owner[owner.index('struct G2GlassesKnob {'):owner.index('// Read the current value of a knob')]
    types = extract_block(owner, 'struct G2LinkSnapshot') + ';\n' + knobs
    # All ordinary BASIC callers must share the rotating ID allocator; the
    # fresh-read helper allocates its named transaction token before sending.
    basic_calls = re.findall(r'g2BuildSettingBasicRequest\([^;]+;', owner)
    assert len(basic_calls) == 4
    assert sum('g2NextSettingsMagic()' in call for call in basic_calls) == 3
    template = (HERE / 'g2_settings_command_harness.cpp').read_text()
    source = template.replace('// INSERT_PROTOCOL', header + '\n' + protocol)
    source = source.replace('// INSERT_TYPES_STATE', types + '\n' + state)
    source = source.replace('// INSERT_OWNER', '\n\n'.join(functions))
    with tempfile.TemporaryDirectory(prefix='hw1-g2-settings-command-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        unit.write_text(source)
        exe = Path(tmp) / 'test'
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-Wno-unused-function', '-Wno-unused-const-variable', '-I', str(COMPONENT),
                   str(unit), '-o', str(exe)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)

if __name__ == '__main__':
    main()
