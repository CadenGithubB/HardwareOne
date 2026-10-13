#!/usr/bin/env python3
"""Compile actual profile/model selection and reject mismatched STT containers.

The 96-byte fixtures are public model envelopes from the pre-change 5x5 file
and the SHA-verified 15x5 gen2 export, not generated from the selected identity.
No weights, external artifacts, device access or ESP-DL execution is needed.
"""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
REPO = COMPONENT.parents[1]
sys.path.insert(0, str(REPO / 'tools'))
import build_config


HEADERS = (
    '48573153545431000100000001000000906f6d0030bb5300803e000000530700'
    'f6b81ad35e808db481b72bcf8ba7c197dfe81836e292926be79bda364fd80df0'
    'ca91eb9683f7151b8be1a01532a6790c6eb7d515f67169a207751e3fc48cc58b',
    '4857315354543100010000000000000000e3320100e33201803e000000530700'
    '47e58faa01470d32dad88f1faa46e3f61dff0fd605aa542103aefd11502dae72'
    'ca91eb9683f7151b8be1a01532a6790c6eb7d515f67169a207751e3fc48cc58b',
)
PATHS = ('/STT Models/quartznet5x5.p4.stt',
         '/sd/STT Models/quartznet15x5-gen2-25397.p4.stt')

HARNESS = r'''
#ifdef TEST_FIRMWARE
#include "System_BuildConfig.h"
#endif
#include "stt/stt_model_selection.h"
#include "stt/stt_model_container.h"
#include <cassert>
#include <cstring>
// INSERT_FIXTURES
int main() {
  using namespace hw1::stt;
  static_assert(identity::kNodeCount == (EXPECT_GEN2 ? 201 : 71));
  static_assert(identity::kRawBytes == (EXPECT_GEN2 ? 20112128 : 7171984));
  static_assert(identity::kCodec == (EXPECT_GEN2 ? 0 : 1));
  static_assert(identity::kOutputExponent == (EXPECT_GEN2 ? -1 : -2));
  assert(std::strcmp(identity::kOutputName, EXPECT_GEN2 ? "529" : "189") == 0);
  assert(std::strcmp(HW1_STT_MODEL_PATH, EXPECT_PATH) == 0);
  constexpr size_t sizes[] = {5487504, 20112224};
  const auto& expected = envelopes[EXPECT_GEN2];
  assert(container::valid_size(sizes[EXPECT_GEN2]));
  assert(container::valid_header(expected, sizeof(expected), sizes[EXPECT_GEN2]));
  // Wrong-model headers fail even when paired with the selected file size.
  assert(!container::valid_header(envelopes[!EXPECT_GEN2], sizeof(expected), sizes[EXPECT_GEN2]));
  assert(!container::valid_size(sizes[!EXPECT_GEN2]));
  // A correct-size container must still match the exact raw model hash.
  uint8_t wrongHash[96]; std::memcpy(wrongHash, expected, sizeof(wrongHash));
  wrongHash[32] ^= 1;
  assert(!container::valid_header(wrongHash, sizeof(wrongHash), sizes[EXPECT_GEN2]));
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit('a host C++17 compiler is required')
    saved = json.loads((REPO / 'configurations/p4-glasses-test.json').read_text())
    build_config.validate(saved)
    assert saved['features']['ENABLE_STT_MODEL_15X5_GEN2'] == 1
    fixtures = 'static const uint8_t envelopes[2][96] = {\n' + ',\n'.join(
        '{' + ','.join(f'0x{x:02x}' for x in bytes.fromhex(header)) + '}'
        for header in HEADERS) + '\n};'
    checks = 0
    with tempfile.TemporaryDirectory(prefix='hw1-stt-model-selection-') as tmp:
        directory = Path(tmp)
        source = directory / 'test.cpp'
        source.write_text(HARNESS.replace('// INSERT_FIXTURES', fixtures))

        def run(name, gen2, features=None, target='ESP32P4', legacy_path=None, error=None):
            nonlocal checks
            command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-I', str(COMPONENT), f'-DEXPECT_GEN2={gen2}',
                       '-DEXPECT_PATH=' + json.dumps(legacy_path or PATHS[gen2])]
            if features is not None:
                overlay = directory / f'{name}.h'
                overlay.write_text('\n'.join(f'#undef {key}\n#define {key} {value}'
                                             for key, value in features.items()))
                board = 'p4x_eye' if target == 'ESP32P4' else 'xiao_s3'
                command += ['-DTEST_FIRMWARE=1', f'-DCONFIG_IDF_TARGET_{target}=1',
                            '-DHW1_BOARD_CONFIG_HEADER=' + json.dumps(str(REPO / f'boards/{board}.features.h')),
                            '-DHW1_FEATURE_CONFIG_HEADER=' + json.dumps(str(overlay))]
                if target == 'ESP32S3':
                    command += ['-DARDUINO_XIAO_ESP32S3_DEV=1']
            if legacy_path:
                command += ['-DHW1_STT_MODEL_PATH=' + json.dumps(legacy_path)]
            if args.sanitize:
                command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            executable = directory / name
            command += [str(source), '-o', str(executable)]
            result = subprocess.run(command, text=True, capture_output=True, timeout=120)
            if error:
                assert result.returncode != 0 and error in result.stderr, result.stderr
            else:
                assert result.returncode == 0, result.stderr
                subprocess.run([str(executable)], check=True, timeout=60)
            checks += 1

        run('portable-default', 0)
        run('board-default', 0, {})
        run('saved-gen2', 1, saved['features'])
        run('saved-disabled', 0, {**saved['features'], 'ENABLE_STT_MODEL_15X5_GEN2': 0})
        run('legacy-path', 1, saved['features'], legacy_path='/sd/STT Models/test-gen2.stt')
        dependency = 'ENABLE_STT_MODEL_15X5_GEN2 requires ENABLE_LOCAL_STT, ESP32-P4 and ENABLE_SD_CARD.'
        run('no-card', 1, {**saved['features'], 'ENABLE_SDMMC_CARD': 0}, error=dependency)
        run('card-forced-off', 1, {**saved['features'], 'ENABLE_SD_CARD': 0}, error=dependency)
        run('no-local-stt', 1, {**saved['features'], 'ENABLE_LOCAL_STT': 0,
                               'ENABLE_STT_SPEAKER_LABELS': 0}, error=dependency)
        run('wrong-target', 1, {'ENABLE_LOCAL_STT': 1, 'ENABLE_SD_CARD': 1,
                                'ENABLE_STT_MODEL_15X5_GEN2': 1}, target='ESP32S3', error=dependency)
    print(f'STT model selection: {checks} profiles passed; exact envelopes accepted, wrong model/hash rejected')


if __name__ == '__main__':
    main()
