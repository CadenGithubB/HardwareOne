#!/usr/bin/env python3
"""Exercise the actual Edge Impulse conversion guard with the shared JPEG parser."""
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
    source = (COMPONENT / 'System_EdgeImpulse.cpp').read_text()
    helper = extract_block(source, 'static bool decodeEdgeImpulseJpeg(')
    # Both production paths must keep the capacity-aware boundary; neither may
    # bypass it or restore a dimension assumption before resize.
    assert source.count('fmt2rgb888(') == 1
    for signature in ('EIResults runEdgeImpulseInference()', 'EIResults runInferenceFromFile('):
        body = extract_block(source, signature)
        assert body.count('decodeEdgeImpulseJpeg(') == 1
        assert 'gRgbBuffer, gRgbBufferSize,' in body
        assert 'cameraWidth' not in body and 'cameraHeight' not in body
        assert 'imgWidth = 640' not in body and 'imgHeight = 480' not in body
        assert 'imgBuffer[1]' not in body
    harness = (HERE / 'edge_impulse_jpeg_harness.cpp').read_text()
    with tempfile.TemporaryDirectory(prefix='hw1-edge-jpeg-') as temporary:
        directory = Path(temporary)
        unit, binary = directory / 'test.cpp', directory / 'test'
        unit.write_text(harness.replace('// INSERT_EDGE_IMPULSE_JPEG_HELPER', helper))
        command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-I', str(COMPONENT), str(unit), str(COMPONENT / 'HAL_JPEG.cpp'),
                   '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-g']
        subprocess.run(command, check=True)
        subprocess.run([str(binary), str(HERE / 'jpeg_fixtures')], check=True)


if __name__ == '__main__':
    main()
