#!/usr/bin/env python3
"""Run real bus reservation/configuration code with only transport/persistence mocked."""
from __future__ import annotations
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
    manager = (COMPONENT / 'System_I2C_Manager.cpp').read_text()
    header = (COMPONENT / 'System_I2C_Manager.h').read_text()
    command = (COMPONENT / 'System_I2C.cpp').read_text()
    # Compile real data layout declarations and constructor mapping; hide only
    # unrelated manager methods. Public visibility lets the test inspect state.
    start = header.index('class I2CDeviceManager {')
    end = header.index('  // Internal helpers', start)
    declaration = header[start:end].replace('private:', 'public:')
    declaration += '\nvoid initBus(uint8_t,int,int,uint32_t);\nbool retainBus(uint8_t);\nvoid releaseBus(uint8_t);\n'+extract_block(header,'  bool busHasClients(')+'\n};\n'
    constants = '\n'.join(re.search(pattern, manager, re.M).group(0) for pattern in (
        r'^#define I2C_WIRE1_DEFAULT_FREQ[^\n]+',
        r'^static constexpr uint32_t I2C_WIRE_IO_TIMEOUT_MS[^\n]+'))
    source = (HERE / 'camera_i2c_reservation_harness.cpp').read_text()
    insertions = {
        '// INSERT_PRODUCTION_REQUEST_TYPES': '\n'.join(
            extract_block(header, marker) + ';' for marker in (
                'enum I2CDeviceType', 'enum class I2CDeviceRequestAction', 'struct I2CDeviceRequest')),
        '// INSERT_PRODUCTION_MANAGER_DECLARATION': declaration,
        '// INSERT_PRODUCTION_MANAGER_CONSTANTS': constants,
        '// INSERT_PRODUCTION_MANAGER_CONSTRUCTOR': extract_block(manager, 'I2CDeviceManager::I2CDeviceManager()'),
        '// INSERT_PRODUCTION_INIT_BUS': '\n'.join(extract_block(manager, sig) for sig in ['void I2CDeviceManager::initBus(', 'bool I2CDeviceManager::retainBus(', 'void I2CDeviceManager::releaseBus(']),
        '// INSERT_PRODUCTION_ENABLE_COMMAND': extract_block(command, 'const char* cmd_i2cbusenabled(') + '\n' + extract_block(command, 'const char* cmd_i2c2busenabled('),
    }
    for marker, definition in insertions.items():
        assert source.count(marker)==1, marker
        source = source.replace(marker, definition)
    with tempfile.TemporaryDirectory(prefix='hw1-camera-i2c-') as directory:
        work=Path(directory)
        generated=work/'test.cpp';generated.write_text(source)
        runtime_variants = 0
        rejected_variants = 0
        boards=(('p4',('HW_BOARD_P4X_EYE=1','CONFIG_IDF_TARGET_ESP32P4=1')),
                ('waveshare',('HW_BOARD_WAVESHARE_P4_TOUCH=1','CONFIG_IDF_TARGET_ESP32P4=1')),
                ('s3',('ARDUINO_XIAO_ESP32S3_DEV=1','CONFIG_IDF_TARGET_ESP32S3=1')),
                ('esp32',('ARDUINO_ADAFRUIT_FEATHER_ESP32_V2_DEV=1','CONFIG_IDF_TARGET_ESP32=1')))
        for enabled in (0,1):
            config=work/f'features-{enabled}.h'
            config.write_text('#undef ENABLE_CAMERA_SENSOR\n#define ENABLE_CAMERA_SENSOR '+str(enabled)+'\n'
                              '#undef ENABLE_LLM_SOURCE_CM5\n#define ENABLE_LLM_SOURCE_CM5 0\n'
                              '#undef ENABLE_RASPBERRY_PI_HOST_POWER\n#define ENABLE_RASPBERRY_PI_HOST_POWER 0\n'
                              '#undef ENABLE_RASPBERRY_PI_HOST_FAN\n#define ENABLE_RASPBERRY_PI_HOST_FAN 0\n')
            for board,definitions in boards:
                for sdk_port1 in (None,0,1):
                    reserved = -1 if not enabled else (1 if board=='waveshare' else (0 if board=='p4' else int(sdk_port1 or 0)))
                    binary=work/f'{board}-{enabled}-{sdk_port1}'
                    flags=['-std=c++17','-Wall','-Wextra','-Werror','-Wno-cpp',
                           '-I',str(COMPONENT),f'-DEXPECTED_CAMERA_ENABLED={enabled}',
                           f'-DEXPECTED_RESERVED_PORT={reserved}',
                           f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{config}"']
                    flags.extend('-D'+item for item in definitions)
                    if sdk_port1 is not None: flags.append(f'-DCONFIG_SCCB_HARDWARE_I2C_PORT1={sdk_port1}')
                    if args.sanitize:flags.extend(['-fsanitize=address,undefined','-g'])
                    compile_command = [args.cxx,*flags,str(generated),'-o',str(binary)]
                    if reserved == 1 and board != 'waveshare':
                        # The shared external bus already owns controller 1.
                        # DVP cannot reserve it while external I2C is enabled.
                        result = subprocess.run(compile_command, capture_output=True, text=True)
                        expected_error = ('External I2C uses controller 1, reserved by camera SCCB; '
                                          'select SCCB I2C0 in sdkconfig or disable ENABLE_CAMERA_SENSOR')
                        assert result.returncode != 0, 'Conflicting camera/external I2C configuration compiled'
                        assert expected_error in result.stderr, result.stderr
                        print(board, 'camera', enabled, 'SDK port1', sdk_port1,
                              'expected controller conflict PASS', flush=True)
                        rejected_variants += 1
                        continue
                    subprocess.run(compile_command, check=True)
                    print(board, 'camera', enabled, 'SDK port1', sdk_port1, flush=True)
                    subprocess.run([str(binary)],check=True)
                    runtime_variants += 1
        # A shared camera cannot borrow a manager omitted from the image.
        config=work/'no-i2c.h'
        config.write_text('#undef ENABLE_CAMERA_SENSOR\n#define ENABLE_CAMERA_SENSOR 1\n'
                          '#undef I2C_FEATURE_LEVEL\n#define I2C_FEATURE_LEVEL 0\n'
                          '#undef INPUT_DEVICE_TYPE\n#define INPUT_DEVICE_TYPE 0\n'
                          '#undef ENABLE_SPEAKER\n#define ENABLE_SPEAKER 0\n'
                          '#undef ENABLE_MICROPHONE_SENSOR\n#define ENABLE_MICROPHONE_SENSOR 0\n')
        result=subprocess.run([args.cxx,'-std=c++17','-fsyntax-only','-x','c++',
                               '-I',str(COMPONENT),'-DHW_BOARD_WAVESHARE_P4_TOUCH=1',
                               '-DCONFIG_IDF_TARGET_ESP32P4=1',
                               f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{config}"','-'],
                              input='#include "System_BuildConfig.h"\n',capture_output=True,text=True)
        assert result.returncode != 0 and 'Shared camera SCCB requires the I2C subsystem' in result.stderr,result.stderr
        rejected_variants += 1
        print(f'CAMERA_I2C_RESERVATION {runtime_variants} runtime variants, '
              f'{rejected_variants} expected compiler rejections PASS', flush=True)


if __name__=='__main__':
    main()
