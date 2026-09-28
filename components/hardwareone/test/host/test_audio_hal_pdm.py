#!/usr/bin/env python3
"""Exercise the production audio HAL's PDM path with two board wirings."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPOSITORY = COMPONENT.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    harness = (HERE / 'audio_hal_pdm_harness.cpp').read_text()
    for marker, filename in (('HAL_HEADER', 'HAL_Audio.h'), ('HAL_SOURCE', 'HAL_Audio.cpp')):
        source = (COMPONENT / filename).read_text()
        source = re.sub(r'^#include[^\n]*', '', source, flags=re.M)
        harness = harness.replace('// INSERT_' + marker, source)
    preferences = []
    for filename, function, end_marker in (
        ('System_Microphone.cpp', 'applyMicPreference', '\n\n  // Capture is owned by HAL_Audio'),
        ('System_ESPSR.cpp', 'applySrPreference', '\n  return audioCaptureStart("sr"'),
    ):
        source = (COMPONENT / filename).read_text()
        start = source.index('  if (gSettings.micSource == "pdm"')
        end = source.index(end_marker, start)
        preferences.append('void ' + function + '() {\n' + source[start:end] + '\n}')
    harness = harness.replace('// INSERT_SOURCE_PREFERENCES', '\n'.join(preferences))
    with tempfile.TemporaryDirectory(prefix='hw1-audio-pdm-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        unit.write_text(harness)
        for powered, clock_limits in ((0, 1), (1, 1), (0, 0)):
            binary = Path(tmp) / f'test-{powered}-{clock_limits}'
            cmd = [args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror',
                   '-Wno-c99-designator', f'-DTEST_POWERED_MIC={powered}',
                   f'-DTEST_MIC_CLOCK_LIMITS={clock_limits}',
                   str(unit), '-o', str(binary)]
            if args.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-g']
            subprocess.run(cmd, check=True)
            subprocess.run([str(binary)], check=True)
        # A board must opt into its own microphone wiring, never inherit XIAO pins.
        result = subprocess.run([args.cxx, '-std=c++17', '-DTEST_POWERED_MIC=0',
                                 '-DTEST_WITH_PINS=0', '-fsyntax-only', str(unit)],
                                text=True, capture_output=True)
        assert result.returncode != 0
        assert 'requires board MIC_CLK_PIN and MIC_DATA_PIN' in result.stderr
        print('Unconfigured microphone board rejected')
        test_board_configuration(args.cxx, Path(tmp))


def test_board_configuration(cxx, directory):
    """Compile the real BuildConfig defaults and deployment override for each board."""
    source = directory / 'board.cpp'
    source.write_text('''#include "System_BuildConfig.h"
static_assert(ENABLE_MICROPHONE_SENSOR == EXPECT_MIC, "board microphone presence");
#ifdef EXPECT_DEPLOYMENT_SENSE
static_assert(ENABLE_MICROPHONE && ENABLE_BLUETOOTH && ENABLE_G2_GLASSES,
              "deployment keeps both local and G2 microphone sources available");
static_assert(XIAO_ESP32S3_SENSE_ENABLED == EXPECT_DEPLOYMENT_SENSE,
              "deployment expansion-board identity");
#endif
#if EXPECT_CLK >= 0
static_assert(MIC_CLK_PIN == EXPECT_CLK && MIC_DATA_PIN == EXPECT_DATA, "board PDM pins");
static_assert(MIC_PDM_LOW_POWER_MAX_HZ == 900000 && MIC_PDM_STANDARD_MIN_HZ == 1100000, "board PDM clock limits");
#else
#ifdef MIC_CLK_PIN
#error "non-microphone board inherited microphone clock wiring"
#endif
#ifdef MIC_DATA_PIN
#error "non-microphone board inherited microphone data wiring"
#endif
#endif
#if EXPECT_POWER >= 0
static_assert(MIC_POWER_PIN == EXPECT_POWER, "board microphone power");
#else
#ifdef MIC_POWER_PIN
#error "board inherited another board's microphone power rail"
#endif
#endif
''')
    boards = (
        ('p4_eye', ('HW_BOARD_P4X_EYE=1', 'CONFIG_IDF_TARGET_ESP32P4=1'), 1, 22, 21, 12),
        ('xiao_sense', ('ARDUINO_XIAO_ESP32S3_DEV=1', 'CONFIG_IDF_TARGET_ESP32S3=1'), 1, 42, 41, -1),
        ('generic_p4', ('HW_BOARD_P4X_EYE=0', 'CONFIG_IDF_TARGET_ESP32P4=1'), 0, -1, -1, -1),
        ('feather_s3', ('ARDUINO_UM_FEATHERS3_DEV=1', 'CONFIG_IDF_TARGET_ESP32S3=1'), 0, -1, -1, -1),
        ('esp32', ('ARDUINO_ADAFRUIT_FEATHER_ESP32_V2_DEV=1', 'CONFIG_IDF_TARGET_ESP32=1'), 0, -1, -1, -1),
    )
    for name, definitions, available, clk, data, power in boards:
        for disabled in (False, True):
            profile = directory / f'{name}-{disabled}.h'
            profile.write_text(
                '#undef ENABLE_LLM_SOURCE_CM5\n#define ENABLE_LLM_SOURCE_CM5 0\n'
                '#undef ENABLE_RASPBERRY_PI_HOST_POWER\n#define ENABLE_RASPBERRY_PI_HOST_POWER 0\n'
                '#undef ENABLE_RASPBERRY_PI_HOST_FAN\n#define ENABLE_RASPBERRY_PI_HOST_FAN 0\n' +
                ('#undef ENABLE_MICROPHONE_SENSOR\n#define ENABLE_MICROPHONE_SENSOR 0\n' if disabled else ''))
            cmd = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-cpp',
                   '-I', str(COMPONENT), '-fsyntax-only', str(source),
                   f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{profile}"',
                   f'-DEXPECT_MIC={0 if disabled else available}', f'-DEXPECT_CLK={clk}',
                   f'-DEXPECT_DATA={data}', f'-DEXPECT_POWER={power}']
            cmd.extend('-D' + value for value in definitions)
            subprocess.run(cmd, check=True)
    print('Audio board wiring and disable overrides: 10 configurations passed')

    # Use the actual deployment headers too: defaults-only checks miss an
    # override that enables the microphone while suppressing its board wiring.
    for target, board, sense in (('p4', boards[0], 0), ('s3', boards[1], 1)):
        _, definitions, available, clk, data, power = board
        profile = REPOSITORY / 'experiments/audio_portable' / f'features-{target}.h'
        cmd = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-cpp',
               '-I', str(COMPONENT), '-fsyntax-only', str(source),
               f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{profile}"',
               f'-DEXPECT_MIC={available}', f'-DEXPECT_CLK={clk}',
               f'-DEXPECT_DATA={data}', f'-DEXPECT_POWER={power}',
               f'-DEXPECT_DEPLOYMENT_SENSE={sense}']
        cmd.extend('-D' + value for value in definitions)
        subprocess.run(cmd, check=True)
    print('Actual audio deployment profiles: P4 and S3 microphone sources/pins passed')


if __name__ == '__main__':
    main()
