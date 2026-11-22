#!/usr/bin/env python3
"""Compile actual profile/model selection and reject mismatched STT containers.

The 96-byte fixtures are public model envelopes from the pre-change 5x5 file,
the SHA-verified 15x5 gen2 export and the packaged Citrinet-384 2000 h export
(tools/stt_p4/package_citrinet.py, repacked), not generated from the selected identity.
Model index: 0 QuartzNet 5x5, 1 QuartzNet 15x5 gen2, 2 Citrinet (the default).
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
    '48573153545431000100000000000000d0615a01d0615a01803e000000530700'
    'a135f830e978dec823f5374f892a58a675aabefecd5b601d1515c8431ccf71e2'
    '4b00ac087270695e7bbc56f0867c995b30bc29323bdda6ca073b03bb5dbb8cc3',
)
PATHS = ('/STT Models/quartznet5x5.p4.stt',
         '/sd/STT Models/quartznet15x5-gen2-25397.p4.stt',
         '/sd/STT Models/citrinet384-2000h-c90.p4.stt')

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
  constexpr int nodes[] = {71, 201, 395}, exponents[] = {-2, -1, -1};
  constexpr size_t raw[] = {7171984, 20112128, 22700496}, sizes[] = {5487504, 20112224, 22700592};
  constexpr unsigned codecs[] = {1, 0, 0};
  const char* outputs[] = {"189", "529", "913"};
  static_assert(identity::kNodeCount == nodes[EXPECT_MODEL]);
  static_assert(identity::kRawBytes == raw[EXPECT_MODEL]);
  static_assert(identity::kCodec == codecs[EXPECT_MODEL]);
  static_assert(identity::kOutputExponent == exponents[EXPECT_MODEL]);
  static_assert(HW1_STT_CITRINET == (EXPECT_MODEL == 2));
#if HW1_STT_CITRINET
  // The repacked image: what a built model still reads ends on a 16-byte boundary inside it.
  static_assert(identity::kResidentKeepBytes == 21384192 && identity::kResidentKeepBytes % 16 == 0);
  static_assert(identity::kResidentKeepBytes < identity::kRawBytes);
#endif
  assert(std::strcmp(identity::kOutputName, outputs[EXPECT_MODEL]) == 0);
  assert(std::strcmp(HW1_STT_MODEL_PATH, EXPECT_PATH) == 0);
  const auto& expected = envelopes[EXPECT_MODEL];
  assert(container::valid_size(sizes[EXPECT_MODEL]));
  assert(container::valid_header(expected, sizeof(expected), sizes[EXPECT_MODEL]));
  // Wrong-model headers fail even when paired with the selected file size.
  for (int other = 0; other < 3; ++other) {
    if (other == EXPECT_MODEL) continue;
    assert(!container::valid_header(envelopes[other], sizeof(expected), sizes[EXPECT_MODEL]));
    assert(!container::valid_size(sizes[other]));
  }
  // A correct-size container must still match the exact raw model hash and front end.
  uint8_t wrongHash[96]; std::memcpy(wrongHash, expected, sizeof(wrongHash));
  wrongHash[32] ^= 1;
  assert(!container::valid_header(wrongHash, sizeof(wrongHash), sizes[EXPECT_MODEL]));
  std::memcpy(wrongHash, expected, sizeof(wrongHash));
  wrongHash[64] ^= 1;
  assert(!container::valid_header(wrongHash, sizeof(wrongHash), sizes[EXPECT_MODEL]));
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
    # The speech profile takes the default model (Citrinet): it sets neither model switch.
    assert not {'ENABLE_STT_MODEL_15X5_GEN2', 'ENABLE_STT_MODEL_CITRINET', 'ENABLE_STT_MODEL_QUARTZNET'} & set(saved['features'])
    # A QuartzNet build is the speech profile with the two model switches turned round.
    quartznet = {'board': saved['board'], 'features': dict(saved['features'], ENABLE_STT_MODEL_CITRINET=0,
                                                           ENABLE_STT_MODEL_QUARTZNET=1)}
    build_config.validate(quartznet)
    fixtures = 'static const uint8_t envelopes[3][96] = {\n' + ',\n'.join(
        '{' + ','.join(f'0x{x:02x}' for x in bytes.fromhex(header)) + '}'
        for header in HEADERS) + '\n};'
    checks = 0
    with tempfile.TemporaryDirectory(prefix='hw1-stt-model-selection-') as tmp:
        directory = Path(tmp)
        source = directory / 'test.cpp'
        source.write_text(HARNESS.replace('// INSERT_FIXTURES', fixtures))

        def run(name, model, features=None, target='ESP32P4', legacy_path=None, error=None):
            nonlocal checks
            command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-I', str(COMPONENT), f'-DEXPECT_MODEL={model}',
                       '-DEXPECT_PATH=' + json.dumps(legacy_path or PATHS[model])]
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

        CITRINET, QUARTZNET_OFF = 2, {'ENABLE_STT_MODEL_CITRINET': 0, 'ENABLE_STT_MODEL_QUARTZNET': 1}
        run('portable-default', 0)
        run('board-default', CITRINET, {})
        run('board-card', CITRINET, {'ENABLE_SDMMC_CARD': 1})
        run('board-card-stt', CITRINET, {'ENABLE_SDMMC_CARD': 1, 'ENABLE_LOCAL_STT': 1})
        run('saved-citrinet', CITRINET, saved['features'])
        # QuartzNet: the board derives the 15x5 gen2 switch from it (boards/p4x_eye.features.h).
        run('saved-quartznet', 1, quartznet['features'])
        run('quartznet-explicit-gen2', 1, {**quartznet['features'], 'ENABLE_STT_MODEL_15X5_GEN2': 1})
        run('legacy-path', CITRINET, saved['features'], legacy_path='/sd/STT Models/test-citrinet.stt')
        run('both-models', CITRINET, {**saved['features'], 'ENABLE_STT_MODEL_QUARTZNET': 1},
            error='Local STT builds one acoustic model')
        run('no-model', CITRINET, {**saved['features'], 'ENABLE_STT_MODEL_CITRINET': 0},
            error='Local STT needs an acoustic model')
        run('gen2-with-citrinet', CITRINET, {**saved['features'], 'ENABLE_STT_MODEL_15X5_GEN2': 1},
            error="ENABLE_STT_MODEL_15X5_GEN2 selects QuartzNet's 15x5 model")
        card = 'Local STT on ESP32-P4 runs its model from the microSD card'
        run('no-card', CITRINET, {**saved['features'], 'ENABLE_SDMMC_CARD': 0}, error=card)
        run('card-forced-off', CITRINET, {**saved['features'], 'ENABLE_SD_CARD': 0}, error=card)
        run('quartznet-gen2-off', 0, {**quartznet['features'], 'ENABLE_STT_MODEL_15X5_GEN2': 0},
            error='A QuartzNet local-STT build on ESP32-P4 runs the 15x5 gen2 model')
        dependency = 'ENABLE_STT_MODEL_15X5_GEN2 requires ENABLE_LOCAL_STT, ESP32-P4 and ENABLE_SD_CARD.'
        run('no-local-stt', 1, {**quartznet['features'], 'ENABLE_STT_MODEL_15X5_GEN2': 1, 'ENABLE_LOCAL_STT': 0,
                               'ENABLE_STT_SPEAKER_LABELS': 0}, error=dependency)
        run('wrong-target', 1, {'ENABLE_LOCAL_STT': 1, 'ENABLE_SD_CARD': 1, **QUARTZNET_OFF,
                                'ENABLE_STT_MODEL_15X5_GEN2': 1}, target='ESP32S3', error=dependency)
    print(f'STT model selection: {checks} profiles passed; exact envelopes accepted, wrong model/hash/front end rejected')


if __name__ == '__main__':
    main()
