#!/usr/bin/env python3
"""Check that an ESP-NOW start that is refused or fails changes no mesh settings.

Compiled part: extracts the production Settings::MeshIdentity record and, from
System_ESPNow.cpp, meshFingerprintForLabel, recomputeAllMeshFingerprints,
initPrimaryMeshFromLegacySettings and the PrimaryMeshBootstrap guard, and runs
start attempts against a Settings double (compiled once with
ENABLE_BONDED_MODE 1 and once with 0): a start that fails leaves slot 0 empty
(no "primary", flags and bond fields as before, a passphrase it held kept, a
key stretched under "primary" during the start dropped), a start that succeeds
creates the enabled default "primary", and a configured or deliberately
emptied slot 0 is never touched.

Source part (initEspNow needs the radio, so its order is pinned in the source):
the "already running" answer and the device-name check come before the first
allocation and before the radio identity cache is cleared; initEspNow no longer
calls the bootstrap itself; the guard is constructed after esp_now_init and
before the callbacks, the passphrase restore and the boot notice; the start
keeps it only on success or when cleanup is still pending; a later start keeps
the peer-array size the arrays were allocated with; and the openespnow reply
does not say "Error:" twice.

--source-dir replays other production sources (for example the tree before the
fix); there the compiled part models the old start, which ran the bootstrap
first and never undid it.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

# Each marker stops before the opening brace: extract_block takes the first "{" after it.
BASE = (
    'static uint16_t meshFingerprintForLabel(const String& label) ',
    'static void recomputeAllMeshFingerprints()',
    'static void initPrimaryMeshFromLegacySettings()',
)
BOOTSTRAP = 'class PrimaryMeshBootstrap '


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow.cpp')
    settings = read('System_Settings.h')

    def definition(text, marker, what):
        count = text.count(marker)
        if count != 1:
            sys.exit(f'FAIL extraction: {what} marker {marker!r} found {count} times, expected 1')
        return extract_block(text, marker)

    # ---- Source checks on initEspNow and cmd_espnow_init -------------------
    checks = 0
    failed = 0

    def check(name, ok):
        nonlocal checks, failed
        checks += 1
        if ok:
            print(f'ok   {name}')
        else:
            failed += 1
            print(f'FAIL {name}')

    def at(text, needle):
        i = text.find(needle)
        return i if i >= 0 else len(text) + 1  # missing sorts after everything

    init = definition(source, 'static bool initEspNow() ', 'System_ESPNow.cpp')
    first_alloc = at(init, 'ps_alloc(')
    already = at(init, 'if (gEspNow && gEspNow->initialized) {')
    name_check = at(init, 'checkEspNowFirstTimeSetup()')
    cache_clear = at(init, 'setEspNowSelfMacCache(nullptr, nullptr);')
    guard = at(init, 'PrimaryMeshBootstrap meshBootstrap;')
    check('A1 "already running" is answered before anything is allocated',
          already < first_alloc and already < cache_clear and
          init.count('broadcastOutput("[ESP-NOW] Already initialized");') == 1)
    check('A2 the device-name check refuses the start before anything is allocated',
          init.count('checkEspNowFirstTimeSetup()') == 1 and name_check < first_alloc and name_check < cache_clear)
    check('A3 initEspNow does not run the slot 0 bootstrap outside the guard',
          'initPrimaryMeshFromLegacySettings()' not in init and 'recomputeAllMeshFingerprints()' not in init)
    check('A4 the guard runs after esp_now_init and before the callbacks are registered',
          init.count('PrimaryMeshBootstrap meshBootstrap;') == 1 and
          at(init, 'esp_now_init()') < guard < at(init, 'esp_now_register_recv_cb('))
    check('A5 the guard runs before the passphrase restore, peer restore and boot notice',
          guard < at(init, '// Restore the encryption passphrase') and
          guard < at(init, 'restoreEspNowPeers();') and guard < at(init, 'buildBootNotification('))
    tail = init[init.rfind('meshBootstrap.keep();'):]
    check('A6 only a successful start keeps "primary" (keep() just before the ON event)',
          init.count('meshBootstrap.keep();') == 3 and
          tail.startswith('meshBootstrap.keep();\n  systemEventPost(SYSEVT_ESPNOW_ON);') and
          tail.count('return') == 1 and 'return true;' in tail)
    check('A7 a failed unwind with cleanup pending keeps slot 0 (a send may still read it)',
          init.count('if (!unwound) meshBootstrap.keep();') == 2)
    check('A8 a later start keeps the peer-array size the arrays were allocated with',
          'if (sMeshPeerArraySlots > 0) slots = sMeshPeerArraySlots;' in init and
          at(init, 'if (sMeshPeerArraySlots > 0) slots = sMeshPeerArraySlots;') < at(init, 'gMeshPeerSlots = slots;') and
          init.count('sMeshPeerArraySlots = slots;') == 1 and
          at(init, 'memset(gMeshPeers, 0, healthSize);') < at(init, 'sMeshPeerArraySlots = slots;') <
          at(init, 'if (!gMeshPeerMeta) {'))
    cmd_init = definition(source, 'const char* cmd_espnow_init(const String& argsInput)', 'System_ESPNow.cpp')
    check('A9 the openespnow reply does not say "Error:" twice',
          'commandReplyReason(gLastInitErrorReason)' in cmd_init)

    # ---- Compiled part ------------------------------------------------------
    have_bootstrap = BOOTSTRAP in source
    mesh_identity = definition(settings, 'struct MeshIdentity', 'System_Settings.h') + ';'
    pieces = [definition(source, marker, 'System_ESPNow.cpp') for marker in BASE]
    if have_bootstrap:
        pieces.append(definition(source, BOOTSTRAP, 'System_ESPNow.cpp') + ';')

    harness = (HERE / 'espnow_start_order_harness.cpp').read_text()
    for tag, value in (('// INSERT_MESH_IDENTITY', mesh_identity),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)
    print(f'slot 0 bootstrap guard: {"present" if have_bootstrap else "absent"}', flush=True)

    compiled_failed = []
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-start-order-') as tmp:
        unit = Path(tmp) / 'espnow_start_order.cpp'
        unit.write_text(harness)
        shutil.copy(a.component / 'test/host/web_batch_stubs/Arduino.h', Path(tmp) / 'Arduino.h')
        for bond in (1, 0):
            exe = Path(tmp) / f'espnow_start_order_bond{bond}'
            cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-function',
                   f'-DENABLE_BONDED_MODE={bond}', f'-DHW1_HAVE_BOOTSTRAP={1 if have_bootstrap else 0}',
                   '-I', tmp, str(unit), '-o', str(exe)]
            if a.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-fno-omit-frame-pointer', '-g']
            subprocess.run(cmd, check=True, timeout=240)
            if subprocess.run([str(exe)], timeout=60).returncode:
                compiled_failed.append(bond)

    print(f'espnow_start_order source checks: {checks} checks, {failed} failed')
    if failed or compiled_failed:
        print('FAIL espnow_start_order' +
              (f': compiled checks failed with ENABLE_BONDED_MODE={compiled_failed}' if compiled_failed else ''),
              file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_start_order')


if __name__ == '__main__':
    main()
