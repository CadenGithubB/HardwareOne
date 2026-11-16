#!/usr/bin/env python3
"""Definitions that must not drift have one owner (2026-11-14 sweep, step 4).

Three sets of values were spelled at every site that uses them, so one edit
could silently split them:

1. The OTA handoff NVS keys "ap_pass" and "auth_token": System_OTA.cpp stores,
   checks and clears them; the separate updater program (updater/main)
   reads them and its PIN path writes them. Owner: hw1_ota_nvs.h
   (HW1_OTA_NVS_AP_PASS_KEY, HW1_OTA_NVS_AUTH_TOKEN_KEY), beside the
   namespace and slot keys both programs already share.
2. The OTA upload frame marker 00 'HW1OTA' 01 and the 12-byte head after which
   the chunk starts: the secure channel (BLE_SC_BINARY_READY), the phone link
   v2 console records (BINARY magic, kBinaryMin) and handleBleUploadFrame must
   classify the same frames. Owner: BLE_OtaWire.h (hw1otawire::kOtaMagic,
   kHeaderSize).
3. The HTTPS key pair paths /system/certs/https_server.crt and .key: HTTPS
   start (WebServer_Utils.cpp) and certinfo/certgen (System_WiFi.cpp), whose
   reply texts quote them. Owner: System_HttpsCertPaths.h
   (HW1_HTTPS_CERT_PATH, HW1_HTTPS_KEY_PATH).

Source contract: each literal appears only in its owner (firmware sources
under components/hardwareone outside test/, and updater/main), and every
former site uses the owner's name. The one documented exception is the web
settings page script (WebPage_Settings.h), which spells the cert paths inside
its browser raw literal.

Value check (no behaviour change): the owners are compiled on the host and
their bytes compared with the values the old code spelled out; the cert reply
texts and staging paths are extracted from production source, compiled
against the owner and compared byte for byte with the old texts.

--root <repository> reads another tree (for example `git archive HEAD`); the
pre-change tree must fail.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

SOURCE_SUFFIXES = {'.c', '.cpp', '.h', '.hpp'}
MARKER = re.compile(r"0x00\s*,\s*'H'\s*,\s*'W'\s*,\s*'1'\s*,\s*'O'\s*,\s*'T'\s*,\s*'A'\s*,\s*0x01")
MARKER_HEX = re.compile(r'0x48\s*,\s*0x57\s*,\s*0x31\s*,\s*0x4[fF]\s*,\s*0x54\s*,\s*0x41')
MARKER_STR = re.compile(r'\\x00HW1OTA|HW1OTA\\x01')
# The values the old code spelled at every site.
OLD_CERT_PATH = b'/system/certs/https_server.crt'
OLD_KEY_PATH = b'/system/certs/https_server.key'
OLD_TEXTS = {
    'certinfo.missing': b'Error: No certificate found at /system/certs/https_server.crt',
    'certgen.keyPath': OLD_KEY_PATH,
    'certgen.certPath': OLD_CERT_PATH,
    'certgen.keyTmp': OLD_KEY_PATH + b'.tmp',
    'certgen.certTmp': OLD_CERT_PATH + b'.tmp',
    'certgen.saved': (b'Self-signed %s certificate generated and saved:\n'
                      b'  Cert: /system/certs/https_server.crt\n'
                      b'  Key:  /system/certs/https_server.key\n'
                      b"Run 'certinfo' to view details.\n"
                      b'Enable HTTPS: httpsenabled 1\n'
                      b'Then reboot to activate.'),
    'web.kHttpsCertPath': OLD_CERT_PATH,
    'web.kHttpsKeyPath': OLD_KEY_PATH,
}


def strip_comments(text: str) -> str:
    text = re.sub(r'/\*.*?\*/', ' ', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def sources(root: Path):
    component = root / 'components' / 'hardwareone'
    for path in sorted(component.rglob('*')):
        if path.suffix in SOURCE_SUFFIXES and path.is_file() and \
                'test' not in path.relative_to(component).parts[:1]:
            yield path
    for path in sorted((root / 'updater' / 'main').glob('*')):
        if path.suffix in SOURCE_SUFFIXES:
            yield path


def contract(root: Path) -> list[str]:
    failures: list[str] = []
    component = root / 'components' / 'hardwareone'
    nvs_owner = root / 'components' / 'hw1_ota_protocol' / 'include' / 'hw1_ota_nvs.h'
    wire_owner = component / 'BLE_OtaWire.h'
    cert_owner = component / 'System_HttpsCertPaths.h'

    def rel(path: Path) -> str:
        return str(path.relative_to(root))

    # 1. Each literal lives only in its owner.
    owners_seen = {'nvs': False, 'marker': False, 'cert': False}
    for path in sources(root):
        text = path.read_text(errors='replace')
        if path == wire_owner:
            owners_seen['marker'] = True
        elif MARKER.search(text) or MARKER_HEX.search(text) or MARKER_STR.search(text):
            failures.append(f'{rel(path)} spells the OTA frame marker; use hw1otawire::kOtaMagic (BLE_OtaWire.h)')
        if path == cert_owner:
            owners_seen['cert'] = True
        elif path.name != 'WebPage_Settings.h':
            for leaf in ('https_server.crt', 'https_server.key'):
                if leaf in text:
                    failures.append(f'{rel(path)} spells {leaf}; use System_HttpsCertPaths.h')
        for key in ('"ap_pass"', '"auth_token"'):
            if key in text:
                failures.append(f'{rel(path)} spells the OTA handoff NVS key {key}; use hw1_ota_nvs.h')
    nvs = nvs_owner.read_text()
    for name, value in (('HW1_OTA_NVS_AP_PASS_KEY', 'ap_pass'), ('HW1_OTA_NVS_AUTH_TOKEN_KEY', 'auth_token')):
        if not re.search(r'^#define ' + name + r' "' + value + r'"$', nvs, re.M):
            failures.append(f'hw1_ota_nvs.h does not own {name} "{value}"')
    if not owners_seen['marker']:
        failures.append('BLE_OtaWire.h (owner of the OTA frame marker and head) is missing')
    else:
        wire = wire_owner.read_text()
        if len(MARKER.findall(wire)) != 1 or 'inline constexpr uint8_t kOtaMagic[8] =' not in wire:
            failures.append('BLE_OtaWire.h must define kOtaMagic once')
        if 'constexpr size_t kHeaderSize = sizeof(kOtaMagic) + kOffsetSize;' not in wire:
            failures.append('BLE_OtaWire.h must define the head length from the marker and the offset')
    if not owners_seen['cert']:
        failures.append('System_HttpsCertPaths.h (owner of the HTTPS key pair paths) is missing')
    else:
        certs = cert_owner.read_text()
        for name, value in (('HW1_HTTPS_CERT_PATH', OLD_CERT_PATH), ('HW1_HTTPS_KEY_PATH', OLD_KEY_PATH)):
            if not re.search(r'^#define ' + name + r' "' + re.escape(value.decode()) + r'"', certs, re.M):
                failures.append(f'System_HttpsCertPaths.h does not own {name}')
        scope = re.search(r'constexpr const char\* CERTS\s*=\s*"([^"]+)"', (component / 'System_VFS.h').read_text())
        if not scope or not all(v.decode().startswith(scope.group(1) + '/') for v in (OLD_CERT_PATH, OLD_KEY_PATH)):
            failures.append('the HTTPS key pair no longer sits under VFS::Scopes::CERTS')

    # 2. Every former site uses the owner's name.
    ota = (component / 'System_OTA.cpp').read_text()
    updater = (root / 'updater' / 'main' / 'updater_main.c').read_text()
    for text, where, each in ((strip_comments(ota), 'System_OTA.cpp', 4), (strip_comments(updater), 'updater_main.c', 2)):
        for name in ('HW1_OTA_NVS_AP_PASS_KEY', 'HW1_OTA_NVS_AUTH_TOKEN_KEY'):
            if len(re.findall(r'\b' + name + r'\b', text)) != each:
                failures.append(f'{where}: expected {each} uses of {name}')
    if '#include "hw1_ota_nvs.h"' not in updater or '#include "hw1_ota_nvs.h"' not in ota:
        failures.append('both programs must include hw1_ota_nvs.h')

    upload = strip_comments(extract_block(ota, 'bool handleBleUploadFrame('))
    if 'memcmp(frame, hw1otawire::kOtaMagic, sizeof(hw1otawire::kOtaMagic))' not in upload or \
            'hw1otawire::kHeaderSize + 1' not in upload or \
            'frame + hw1otawire::kHeaderSize' not in upload or 'size - hw1otawire::kHeaderSize' not in upload:
        failures.append('handleBleUploadFrame does not take the marker and head length from BLE_OtaWire.h')
    if re.search(r'\bkBleUploadHeaderSize\b', ota):
        failures.append('System_OTA.cpp keeps its own copy of the OTA frame head length')
    secure = (component / 'BLE_SecureChannel.cpp').read_text()
    inbound = strip_comments(extract_block(secure, 'BleScResult bleScHandleInbound('))
    if 'kOtaPrefix' in secure or \
            'memcmp(out, hw1otawire::kOtaMagic, sizeof(hw1otawire::kOtaMagic)) == 0' not in inbound:
        failures.append('BLE_SecureChannel.cpp does not classify BINARY_READY with hw1otawire::kOtaMagic')
    if '#include "BLE_OtaWire.h"' not in secure or '#include "BLE_OtaWire.h"' not in ota:
        failures.append('the secure channel and the OTA handler must include BLE_OtaWire.h')
    core = strip_comments((component / 'BLE_PhonePipeConsoleCore.h').read_text())
    if 'using hw1otawire::kOtaMagic;' not in core or 'kBinaryMin = hw1otawire::kHeaderSize + 1' not in core:
        failures.append('BLE_PhonePipeConsoleCore.h does not take kOtaMagic and kBinaryMin from BLE_OtaWire.h')

    web = (component / 'WebServer_Utils.cpp').read_text()
    for name, macro in (('kHttpsCertPath', 'HW1_HTTPS_CERT_PATH'), ('kHttpsKeyPath', 'HW1_HTTPS_KEY_PATH')):
        if not re.search(r'const char\* const ' + name + r'\s*=\s*' + macro + r';', web):
            failures.append(f'WebServer_Utils.cpp: {name} is not {macro}')
    wifi = (component / 'System_WiFi.cpp').read_text()
    info = strip_comments(extract_block(wifi, 'const char* cmd_certinfo(const String& argsInput)'))
    if 'CERT_PATH =' in info or 'existsGuarded(HW1_HTTPS_CERT_PATH,' not in info or \
            'openGuarded(HW1_HTTPS_CERT_PATH,' not in info:
        failures.append('cmd_certinfo does not open the certificate through HW1_HTTPS_CERT_PATH')
    gen = strip_comments(extract_block(wifi, 'const char* cmd_certgen(const String& argsInput)'))
    for var, macro in (('keyPath', 'HW1_HTTPS_KEY_PATH'), ('certPath', 'HW1_HTTPS_CERT_PATH'),
                       ('keyTmp', 'HW1_HTTPS_KEY_PATH'), ('certTmp', 'HW1_HTTPS_CERT_PATH')):
        if not re.search(r'const String ' + var + r'\s*=\s*' + macro + r'\b', gen):
            failures.append(f'cmd_certgen: {var} is not built from {macro}')
    if '"  Cert: " HW1_HTTPS_CERT_PATH' not in gen or '"  Key:  " HW1_HTTPS_KEY_PATH' not in gen:
        failures.append('the certgen reply does not quote the paths through the owner')
    return failures


def compile_and_run(cxx: str, work: Path, name: str, code: str, includes: list[Path], lang: str = 'c++') -> bytes:
    src = work / f'{name}.{"cpp" if lang == "c++" else "c"}'
    exe = work / name
    src.write_text(code)
    std = '-std=c++17' if lang == 'c++' else '-std=c11'
    command = [cxx, '-x', lang, std, '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)]
    for include in includes:
        command[1:1] = ['-I', str(include)]
    subprocess.run(command, check=True)
    return subprocess.run([str(exe)], check=True, stdout=subprocess.PIPE).stdout


def values(root: Path, cxx: str) -> list[str]:
    failures: list[str] = []
    component = root / 'components' / 'hardwareone'
    wifi = (component / 'System_WiFi.cpp').read_text()
    web = (component / 'WebServer_Utils.cpp').read_text()
    info = extract_block(wifi, 'const char* cmd_certinfo(const String& argsInput)')
    gen = extract_block(wifi, 'const char* cmd_certgen(const String& argsInput)')
    expressions = {}
    found = re.search(r'return\s+("Error: No certificate found at [^;]*);', info)
    expressions['certinfo.missing'] = found and found.group(1)
    for var in ('keyPath', 'certPath', 'keyTmp', 'certTmp'):
        found = re.search(r'const String ' + var + r'\s*=\s*([^;]+);', gen)
        expressions[f'certgen.{var}'] = found and found.group(1)
    found = re.search(r'snprintf\(gDebugBuffer,\s*1024,\s*("Self-signed .*?),\s*useRsa \?', gen, re.S)
    expressions['certgen.saved'] = found and found.group(1)
    for name in ('kHttpsCertPath', 'kHttpsKeyPath'):
        found = re.search(r'const char\* const ' + name + r'\s*=\s*([^;]+);', web)
        expressions[f'web.{name}'] = found and found.group(1)
    missing = [k for k, v in expressions.items() if not v]
    if missing:
        return [f'could not extract {", ".join(missing)}']

    with tempfile.TemporaryDirectory(prefix='hw1-definition-owners-') as tmp:
        work = Path(tmp)
        stubs = work / 'stubs'
        stubs.mkdir()
        (stubs / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n')
        include = '#include "System_HttpsCertPaths.h"\n' if (component / 'System_HttpsCertPaths.h').exists() else ''
        lines = ''.join(f'  put({expr});\n' for expr in expressions.values())
        out = compile_and_run(cxx, work, 'certs', f'''{include}#include <cstdio>
#include <cstring>
static void put(const char* s) {{ std::fwrite(s, 1, std::strlen(s) + 1, stdout); }}
int main() {{
{lines}  return 0;
}}
''', [component])
        got = out.split(b'\0')[:-1]
        for (name, old), new in zip(OLD_TEXTS.items(), got):
            if new != old:
                failures.append(f'{name} changed: {new!r} != {old!r}')
        if len(got) != len(OLD_TEXTS):
            failures.append(f'expected {len(OLD_TEXTS)} cert texts, got {len(got)}')

        if (component / 'BLE_OtaWire.h').exists():
            out = compile_and_run(cxx, work, 'wire', '''#include "BLE_PhonePipeConsoleCore.h"
#include "BLE_OtaWire.h"
#include <cstdio>
static_assert(&hw1rec::kOtaMagic[0] == &hw1otawire::kOtaMagic[0], "the record layer uses the owner's marker");
int main() {
  for (unsigned i = 0; i < sizeof(hw1otawire::kOtaMagic); ++i) std::printf("%02x", hw1otawire::kOtaMagic[i]);
  std::printf(" %zu %zu %zu\\n", sizeof(hw1rec::kOtaMagic), hw1otawire::kHeaderSize, hw1rec::kBinaryMin);
  return 0;
}
''', [component])
            if out != b'004857314f544101 8 12 13\n':
                failures.append(f'OTA frame marker/head changed: {out!r}')

        out = compile_and_run(cxx, work, 'nvs', '''#include "hw1_ota_nvs.h"
#include <stdio.h>
int main(void) {
#if defined(HW1_OTA_NVS_AP_PASS_KEY) && defined(HW1_OTA_NVS_AUTH_TOKEN_KEY)
  printf("%s|%s|%s\\n", HW1_OTA_NVS_NAMESPACE, HW1_OTA_NVS_AP_PASS_KEY, HW1_OTA_NVS_AUTH_TOKEN_KEY);
#else
  printf("%s|(no owner)\\n", HW1_OTA_NVS_NAMESPACE);
#endif
  return 0;
}
''', [stubs, root / 'components' / 'hw1_ota_protocol' / 'include'], lang='c')
        if out != b'hw1up|ap_pass|auth_token\n':
            failures.append(f'OTA handoff NVS keys changed: {out!r}')
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--root', type=Path, default=HERE.parents[3], help='repository root to read')
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    args = parser.parse_args()
    root = args.root.resolve()
    failures = contract(root) + values(root, args.cxx)
    if failures:
        for failure in failures:
            print(f'definition_owners: {failure}', file=sys.stderr)
        return 1
    print('definition_owners: OTA handoff NVS keys, OTA frame marker/head and HTTPS key pair paths '
          'each have one owner; values and reply texts unchanged')
    return 0


if __name__ == '__main__':
    sys.exit(main())
