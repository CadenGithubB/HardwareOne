#!/usr/bin/env python3
"""X3: passphrase entry parity between Android and the P4 serial command.

Runs phone_passphrase_vectors.cpp (production String, command split,
`phone secret` handler, internal secret storage and PBKDF2) and compares the
result with tools/ble_secure/fixtures/phone_pipe_v1_passphrase.json.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from arduino_string_host import arduino_string
from phone_crypto_build import crypto_flags
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
IS_CODE = r"""
#include "BLE_PhonePairCode.h"
#include <cstdio>
#include <string>
// One hex line per text on stdin; prints 1 when hw1code::isCode accepts it.
int main() {
  char line[1024];
  while (fgets(line, sizeof line, stdin)) {
    std::string bytes;
    for (size_t i = 0; line[i] && line[i] != '\n' && line[i + 1]; i += 2)
      bytes += char(std::stoi(std::string(line + i, 2), nullptr, 16));
    printf("%d\n", hw1code::isCode(bytes.data(), bytes.size()) ? 1 : 0);
  }
}
"""
COMPONENT = HERE.parents[1]
FIXTURE = HERE.parents[3] / 'tools/ble_secure/fixtures/phone_pipe_v1_passphrase.json'


def fixture(vectors):
    return {
        'description': 'HW1-PIPE-v1 X3: the same text entered in the Android app and typed into the P4 as '
                       '"phone secret <text>" over serial. Synthetic passphrases only.',
        'generated_by': 'components/hardwareone/test/host/phone_passphrase_vectors.cpp: production Arduino String, '
                        'System_CommandLookupCore.h, phoneSecretCommand and blePassphrasePolicyError '
                        '(Bluetooth.cpp), setInternalSecretSetting and scPbkdf2',
        'path': [
            'Android: entry rule from DECISIONS Q6/A-7/A-9 (reject NUL, CR, LF and invalid UTF-8; trim outer '
            'ASCII 09-0D and 20; 10-128 bytes; ASCII uppercase, lowercase, digit and punctuation required)',
            'P4 serial reader (HardwareOne.cpp loop drain): every CR is dropped, LF ends the line',
            'P4 executor (System_Utils.cpp): the line becomes a C string, so it ends at the first NUL',
            'P4 command split (System_CommandLookupCore.h) and String::trim on the command and its argument',
            'phoneSecretCommand: trim; "clear" in any case clears; empty reports status; over 128 bytes '
            'rejected; strength policy via C-locale ctype on unsigned bytes; no UTF-8 validation',
            'PSK = PBKDF2-HMAC-SHA256(stored bytes, "HW1-PIPE-v1", 100000, 32)',
        ],
        'assumptions': [
            'Target: RISC-V, plain char unsigned (generator built with -funsigned-char)',
            'Target newlib C locale classifies only ASCII; the generator asserts the host C locale matches '
            'for all 256 byte values',
            'The terminal sends CR LF after the line',
        ],
        'parity': 'same_secret = Android accepts and the P4 stores identical bytes (checked by the generator); '
                  'android_rejects = outside the supported entry path, P4 behaviour recorded for reference',
        'vectors': vectors,
    }


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--idf', type=Path)
    parser.add_argument('--crypto-build', type=Path, help='Reuse an already built native crypto directory')
    parser.add_argument('--write-vectors', action='store_true', help='Explicitly regenerate the fixture')
    args = parser.parse_args()
    bt = (COMPONENT / 'Bluetooth.cpp').read_text()
    settings = (COMPONENT / 'System_Settings.cpp').read_text()
    utils = (COMPONENT / 'System_Utils.cpp').read_text()
    production = '\n'.join([extract_block(utils, 'void secureClearString('),
                            extract_block(settings, 'bool setInternalSecretSetting('),
                            extract_block(bt, 'static const char* blePassphrasePolicyError('),
                            extract_block(bt, 'const char* phonePipeReplaceSecret('),
                            extract_block(bt, 'const char* phoneSecretCommand(')])
    unit = (HERE / 'phone_passphrase_vectors.cpp').read_text().replace('// PRODUCTION', production)
    sanitize = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g'] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix='hw1-pipe-passphrase-') as tmp, \
            crypto_flags(args.idf, args.crypto_build) as (crypto_includes, libraries):
        d = Path(tmp)
        (d / 'vectors.cpp').write_text(unit)
        includes, sources = arduino_string(d, ['-funsigned-char', *sanitize])
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-funsigned-char', '-Wall', '-Wextra',
                        '-Werror', '-Wno-sign-compare', *sanitize, *includes, '-I', str(COMPONENT),
                        '-I', str(HERE / 'phone_gatt_stubs'), *crypto_includes, str(d / 'vectors.cpp'),
                        *sources, *libraries, '-o', str(d / 'vectors')], check=True)
        vectors = [json.loads(line) for line in
                   subprocess.check_output([str(d / 'vectors')], text=True).splitlines()]
        # Phone link v2: `phone secret <text>` stores a pairing code's canonical
        # form instead of the text, so no X3 text may parse as a code (else X3
        # would change). Every form the P4 or Android could see is checked with
        # the production parser: as typed, cut at NUL, without CR, and the bytes
        # each side stores.
        (d / 'is_code.cpp').write_text(IS_CODE)
        subprocess.run([os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', *sanitize,
                        '-I', str(COMPONENT), str(d / 'is_code.cpp'), '-o', str(d / 'is_code')], check=True)
        forms = []
        for v in vectors:
            text = bytes.fromhex(v['text_hex'])
            forms += [text, text.split(b'\0')[0], text.replace(b'\r', b'')]
            forms += [bytes.fromhex(v[k]) for k in ('android_secret_hex', 'p4_stored_hex') if v[k] is not None]
        verdicts = subprocess.run([str(d / 'is_code')], input=''.join(f.hex() + '\n' for f in forms), text=True,
                                  capture_output=True, check=True).stdout.split()
        assert len(verdicts) == len(forms) and set(verdicts) == {'0'}, 'an X3 text parses as a pairing code'
        assert subprocess.run([str(d / 'is_code')], input=b'0123-4567-89ab-cde4\n'.hex() + '\n', text=True,
                              capture_output=True, check=True).stdout.split() == ['1']
    actual = fixture(vectors)
    if args.write_vectors:
        FIXTURE.write_text(json.dumps(actual, indent=2, ensure_ascii=False) + '\n')
    expected = json.loads(FIXTURE.read_text())
    # X3 is frozen and copied into Android: entry bytes and derived keys must
    # stay identical. Only the successful CLI guidance changed with the shared
    # Bluetooth lifecycle. Check the new wording without rewriting that pin.
    old_guidance = 'Pipe secret set; enter the same passphrase on Android, then phone start and phone pair'
    new_guidance = 'Pipe secret set; enter the same passphrase on Android, then openphone and phone pair'
    for vector in expected['vectors']:
        if vector['p4_result'] == 'set' and vector['p4_message'] == old_guidance:
            vector['p4_message'] = new_guidance
    assert actual == expected, 'Production passphrase path differs from the X3 fixture'
    same = sum(v['parity'] == 'same_secret' for v in vectors)
    print(f'Passphrase parity: {same} Android-accepted texts store identical bytes on the P4; '
          f'{len(vectors) - same} rejected cases recorded; matches the X3 fixture; no X3 text is a pairing code')
