#!/usr/bin/env python3
"""Build and run stt_dl_metadata_harness.cpp against the real ESP-DL metadata code.

    test_stt_dl_metadata.py [--esp-dl DIR] [--sanitize]

ESP-DL comes from the staged P4 override (.sdk-overrides/esp32p4/espressif__esp-dl,
tools/p4/prepare_sdk.sh) unless --esp-dl names another patched copy; without one the
test is skipped (exit 77). TensorBase's destructor, both constructors, set_shape and
dtype_sizeof are extracted unchanged from dl_tensor_base.cpp (tensor_extract.inc) and
compiled with dl_tool_metadata.cpp, dl_model_context.cpp and
components/hardwareone/stt/stt_dl_metadata.cpp; the ESP-IDF heap and System_MemUtil's
ps_alloc/ps_free are fakes (dl_metadata_stubs/fake_heap.cpp).
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parents[1]
REPO = HERE.parents[3]
STUBS = HERE / 'dl_metadata_stubs'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--esp-dl', type=Path, default=REPO / '.sdk-overrides/esp32p4/espressif__esp-dl')
    parser.add_argument('--sanitize', action='store_true', help='AddressSanitizer and UBSan')
    args = parser.parse_args()
    dl = args.esp_dl
    if not (dl / 'dl/tool/src/dl_tool_metadata.cpp').is_file():
        print(f'SKIP: no patched ESP-DL at {dl} (run tools/p4/prepare_sdk.sh)')
        sys.exit(77)
    src = (dl / 'dl/tensor/src/dl_tensor_base.cpp').read_text()

    def function(signature):
        i = src.index(signature)
        j = src.index('{', i)
        depth = 0
        for k in range(j, len(src)):
            depth += {'{': 1, '}': -1}.get(src[k], 0)
            if depth == 0:
                return src[i:k + 1] + '\n'
        raise SystemExit('unbalanced ' + signature)

    parts = [function('size_t dtype_sizeof(dtype_t dtype)'),
             function('TensorBase::~TensorBase()'),
             function('TensorBase::TensorBase(\n    std::vector<int> shape, const void *element, int exponent'),
             function('TensorBase::TensorBase(std::vector<int> shape,\n                       const void *element,\n'
                      '                       const std::vector<int> &exponents'),
             function('TensorBase &TensorBase::set_shape(const std::vector<int> shape)')]
    with tempfile.TemporaryDirectory(prefix='hw1-dl-metadata-') as tmp:
        (Path(tmp) / 'tensor_extract.inc').write_text('// extracted unchanged from dl_tensor_base.cpp\n' + ''.join(parts))
        exe = Path(tmp) / 'test'
        cmd = ['clang++', '-std=c++17', '-g', '-O1', '-Wall', '-Wextra', '-Werror',
               # upstream ESP-DL headers are not warning-clean on a 64-bit host
               '-Wno-sign-compare', '-Wno-string-plus-int', '-Wno-format', '-Wno-unused-parameter',
               '-Wno-unused-variable', '-Wno-unused-function', '-Wno-deprecated-declarations',
               '-Wno-missing-field-initializers', '-Wno-unused-private-field',
               '-I', tmp, '-I', str(STUBS),
               '-I', str(dl / 'dl'), '-I', str(dl / 'dl/tool/include'), '-I', str(dl / 'dl/tensor/include'),
               '-I', str(dl / 'dl/model/include'), '-I', str(SOURCE / 'stt'),
               str(HERE / 'stt_dl_metadata_harness.cpp'), str(STUBS / 'fake_heap.cpp'),
               str(dl / 'dl/tool/src/dl_tool_metadata.cpp'), str(dl / 'dl/model/src/dl_model_context.cpp'),
               str(SOURCE / 'stt/stt_dl_metadata.cpp'), '-o', str(exe), '-lpthread']
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=undefined']
        subprocess.run(cmd, check=True, timeout=300)
        subprocess.run([str(exe)], check=True, timeout=300)


if __name__ == '__main__':
    main()
