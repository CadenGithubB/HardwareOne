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
    declaration += '\nvoid initBus(uint8_t,int,int,uint32_t);\n};\n'
    constants = '\n'.join(re.search(pattern, manager, re.M).group(0) for pattern in (
        r'^#define I2C_WIRE1_DEFAULT_FREQ[^\n]+',
        r'^static constexpr uint32_t I2C_WIRE_IO_TIMEOUT_MS[^\n]+'))
    source = (HERE / 'camera_i2c_reservation_harness.cpp').read_text()
    insertions = {
        '// INSERT_PRODUCTION_MANAGER_DECLARATION': declaration,
        '// INSERT_PRODUCTION_MANAGER_CONSTANTS': constants,
        '// INSERT_PRODUCTION_MANAGER_CONSTRUCTOR': extract_block(manager, 'I2CDeviceManager::I2CDeviceManager()'),
        '// INSERT_PRODUCTION_INIT_BUS': extract_block(manager, 'void I2CDeviceManager::initBus('),
        '// INSERT_PRODUCTION_ENABLE_COMMAND': extract_block(command, 'const char* cmd_i2cbusenabled(') + '\n' + extract_block(command, 'const char* cmd_i2c2busenabled('),
    }
    for marker, definition in insertions.items():
        assert source.count(marker)==1, marker
        source = source.replace(marker, definition)
    with tempfile.TemporaryDirectory(prefix='hw1-camera-i2c-') as directory:
        work=Path(directory)
        generated=work/'test.cpp';generated.write_text(source)
        boards=(('p4',('HW_BOARD_P4X_EYE=1','CONFIG_IDF_TARGET_ESP32P4=1')),
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
                    reserved = -1 if not enabled else (0 if board=='p4' else int(sdk_port1 or 0))
                    binary=work/f'{board}-{enabled}-{sdk_port1}'
                    flags=['-std=c++17','-Wall','-Wextra','-Werror','-Wno-cpp',
                           '-I',str(COMPONENT),f'-DEXPECTED_CAMERA_ENABLED={enabled}',
                           f'-DEXPECTED_RESERVED_PORT={reserved}',
                           f'-DHW1_DEPLOYMENT_CONFIG_HEADER="{config}"']
                    flags.extend('-D'+item for item in definitions)
                    if sdk_port1 is not None: flags.append(f'-DCONFIG_SCCB_HARDWARE_I2C_PORT1={sdk_port1}')
                    if args.sanitize:flags.extend(['-fsanitize=address,undefined','-g'])
                    subprocess.run([args.cxx,*flags,str(generated),'-o',str(binary)],check=True)
                    print(board, 'camera', enabled, 'SDK port1', sdk_port1, flush=True)
                    subprocess.run([str(binary)],check=True)


if __name__=='__main__':
    main()
