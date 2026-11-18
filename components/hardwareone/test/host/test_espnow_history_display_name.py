#!/usr/bin/env python3
"""Run the production espnowHistoryDisplayName resolver against doubles.

History records carry no name; readers (web, CLI, OLED, G2) name a row with
espnowHistoryDisplayName() (System_ESPNow.cpp). This suite extracts that
function as shipped, plus MESH_PEER_MAX, the ESPNOW_HISTORY_ macros,
LogMessageType and ReceivedTextMessage from System_ESPNow.h, and runs it in
espnow_history_display_name_harness.cpp against doubles of the mesh metadata
table, the paired registry and GroupLockGuard:

  order         active metadata name, then the registry name, else the MAC
                text; an empty or inactive metadata slot falls through; a
                rename shows at once
  kinds         sent -> "", unattributed -> MAC text, neither takes the lock
  unterminated  32-byte names with no NUL are read bounded
  caps          cap 0 / null out write nothing; cap 1, 2, 3 truncate and
                terminate
  null_tables   gMeshPeerMeta / gEspNow null, ESP-NOW closed
  bounds        gMeshPeerSlots and deviceCount past their arrays are clamped
  page          espnowHistoryPageNames (the espnowmessages page): one lookup
                per distinct attributed peer, reused only between attributed
                received rows of the same peer; sent and unattributed rows
                never lend or borrow a name

Source contracts: the doubles' field sizes match the header (MeshPeerMeta
name[32], EspNowDevice EspNowInlineText<32> name, devices[16]); the resolver
takes GroupLockGuard and uses no String; cmd_espnow_messages names its page
through espnowHistoryPageNames.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
RESOLVER = 'void espnowHistoryDisplayName(const ReceivedTextMessage& m, char* out, size_t cap) '
PAGE = 'static void espnowHistoryPageNames(const ReceivedTextMessage* msgs, int n, char (*names)[ESPNOW_HISTORY_NAME_CAP]) '


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true', help='AddressSanitizer + UBSan')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    header = (a.component / 'System_ESPNow.h').read_text()
    source = (a.component / 'System_ESPNow.cpp').read_text()
    failures = []

    meta = extract_block(header, 'struct MeshPeerMeta {')
    if not re.search(r'^\s*char name\[32\];', meta, re.M):
        failures.append('MeshPeerMeta::name is no longer char[32] (update the double)')
    dev = extract_block(header, 'struct EspNowDevice {')
    if not re.search(r'^\s*EspNowInlineText<32> name;', dev, re.M):
        failures.append('EspNowDevice::name is no longer EspNowInlineText<32> (update the double)')
    if len(re.findall(r'^\s*EspNowDevice devices\[16\];', header, re.M)) != 1:
        failures.append('EspNowState::devices is no longer [16] (update the double)')
    resolver = extract_block(source, RESOLVER)
    if 'GroupLockGuard' not in resolver or 'String' in re.sub(r'//[^\n]*', '', resolver):
        failures.append('espnowHistoryDisplayName must take GroupLockGuard and use no String')

    api = [line for line in header.splitlines()
           if line.startswith('#define MESH_PEER_MAX ') or line.startswith('#define ESPNOW_HISTORY_NAME_CAP')]
    api.append(extract_block(header, 'enum LogMessageType') + ';')
    api.append(extract_block(header, 'struct ReceivedTextMessage') + ';')
    api.append(RESOLVER.strip() + ';')
    page = extract_block(source, PAGE)
    cli = extract_block(source, 'const char* cmd_espnow_messages(const String& argsInput) ')
    if 'espnowHistoryPageNames(msgs, n, names);' not in cli:
        failures.append('cmd_espnow_messages does not name its page through espnowHistoryPageNames')

    harness = (HERE / 'espnow_history_display_name_harness.cpp').read_text()
    for tag, value in (('// INSERT_API', '\n'.join(api)), ('// INSERT_RESOLVER', resolver + '\n\n' + page)):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    with tempfile.TemporaryDirectory(prefix='hw1-espnow-names-') as tmp:
        unit = Path(tmp) / 'display_name.cpp'
        unit.write_text(harness)
        exe = Path(tmp) / 'display_name'
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', f'-I{a.component}', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=180)
        r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
        sys.stdout.write(r.stdout)
        if r.stderr:
            sys.stderr.write(r.stderr[:4000])
        if r.returncode != 0:
            failures.append(f'harness exit {r.returncode}')
    for f in failures:
        print('FAIL', f, file=sys.stderr)
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
