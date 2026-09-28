#!/usr/bin/env python3
"""Compile production portable JPEG code and optionally the installed TJpgDec.

The default suite mocks only codec/SDK/heap boundaries. --jpeg-component and
--camera-component additionally compile the real software decoder and legacy
fmt2rgb888 wrapper, checking exact pixels without requiring a device or Pillow.
"""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
STUBS = HERE / 'jpeg_stubs'
FIXTURES = HERE / 'jpeg_fixtures'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--cc', default=os.environ.get('CC') or shutil.which('cc'))
    parser.add_argument('--jpeg-component', type=Path)
    parser.add_argument('--camera-component', type=Path)
    args = parser.parse_args()
    if bool(args.jpeg_component) != bool(args.camera_component):
        parser.error('--jpeg-component and --camera-component must be provided together')
    cxx = args.cxx
    cc = args.cc
    if not cxx or not cc:
        parser.error('host C and C++ compilers are required')
    flags = ['-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-jpeg-host-') as work:
        work = Path(work)
        def compile_run(name, source, sources, extra=()):
            binary = work / name
            subprocess.run([cxx, '-std=c++17', *flags, *extra, '-I', str(STUBS), '-I', str(COMPONENT),
                            str(HERE / source), *(str(COMPONENT / x) for x in sources),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary), str(FIXTURES)], check=True)
        compile_run('core', 'test_jpeg_core.cpp', ['HAL_JPEG.cpp'])
        helper = extract_block((COMPONENT / 'G2_Glasses.cpp').read_text(), 'static bool decodeJpegForG2(')
        software_source = work / 'jpeg_software_with_g2.cpp'
        software_source.write_text((HERE / 'test_jpeg_software.cpp').read_text().replace('// INSERT_G2_JPEG_HELPER', helper))
        compile_run('software', software_source, ['HAL_JPEG.cpp', 'HAL_JPEG_Software.cpp'])
        for capability, qualified in ((0, 0), (1, 0), (1, 1)):
            compile_run(f'hardware-{capability}-{qualified}', 'test_jpeg_hardware.cpp',
                        ['HAL_JPEG.cpp', 'HAL_JPEG_P4.cpp'],
                        [f'-DSOC_JPEG_DECODE_SUPPORTED={capability}', f'-DHW1_JPEG_DRIVER_QUALIFIED={qualified}'])
        if args.jpeg_component:
            jpeg = args.jpeg_component.resolve()
            camera = args.camera_component.resolve()
            includes = ['-I', str(jpeg / 'include'), '-I', str(jpeg / 'tjpgd'), '-I', str(STUBS)]
            # ESP firmware size_t is 32-bit; desktop size_t is 64-bit. Adapt only
            # the wrapper input callback signature to TJpgDec's size_t API. The
            # wrapper body, decoder, legacy converter and codec algorithms stay
            # unchanged. This is not a decoder substitute or golden re-encode.
            wrapper = (jpeg / 'jpeg_decoder.c').read_text()
            wrapper = wrapper.replace('static unsigned int jpeg_decode_in_cb(',
                                      'static size_t jpeg_decode_in_cb(')
            wrapper = wrapper.replace('uint8_t *buff, unsigned int nbyte)',
                                      'uint8_t *buff, size_t nbyte)')
            adapted = work / 'jpeg_decoder_host.c'
            adapted.write_text(wrapper)
            for optimization in (0, 1, 2):
                objects = []
                for index, source in enumerate((adapted, jpeg / 'tjpgd/tjpgd.c', jpeg / 'jpeg_default_huffman_table.c', camera / 'conversions/to_bmp.c')):
                    obj = work / f'decoder-{optimization}-{index}.o'
                    # Third-party C uses GNU/C extensions and unused legacy locals.
                    cflags = [flag for flag in flags if flag not in ('-pedantic', '-Werror')]
                    subprocess.run([cc, '-std=gnu11', *cflags, f'-DCONFIG_JD_FASTDECODE={optimization}', *includes,
                                    '-c', str(source), '-o', str(obj)], check=True)
                    objects.append(str(obj))
                compile_run(f'parity-{optimization}', 'test_jpeg_parity.cpp',
                            ['HAL_JPEG.cpp', 'HAL_JPEG_Software.cpp', 'HAL_JPEG_P4.cpp'],
                            ['-DSOC_JPEG_DECODE_SUPPORTED=0', f'-DCONFIG_JD_FASTDECODE={optimization}', '-pthread', *includes, *objects])
        else:
            print('Actual decoder corpus parity was not requested (supply both component paths).')


if __name__ == '__main__':
    main()
