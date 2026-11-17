#!/usr/bin/env python3
"""Run the actual ESP-NOW mesh-slot commands and the device-name command.

Extracts the production Settings::MeshIdentity record and, from
System_ESPNow.cpp, meshFingerprintForLabel, setEspNowPassphrase,
initPrimaryMeshFromLegacySettings, the start-passphrase and hint helpers, the
espnowmeshes subcommands (add, remove, delete, enable, setdefault,
setpassphrase) and cmd_espnow_setname, and runs them against host doubles for
settings writes, the key cache, stored identities, events and the ESP-NOW state.

Checks that a device's only mesh can be disabled and deleted (back to the
unconfigured "primary" mesh of a new device), that delete frees a slot for 'add'
and is refused while paired devices still name it, that exactly one enabled mesh
is the default, that the passphrase messages name the mesh by its own label, and
that the device name can be cleared while ESP-NOW is stopped. --source-dir
replays other production sources (for example the pre-fix tree) against this
suite.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

REQUIRED = (
    'static uint16_t meshFingerprintForLabel(const String& label) ',
    'static void setEspNowPassphrase(const String& passphrase)',
    'static void initPrimaryMeshFromLegacySettings()',
    'static bool isValidMeshLabel(',
    'static int findFreeMeshSlot()',
    'static const char* meshesCmd_add(',
    'static const char* meshesCmd_remove(',
    'static const char* meshesCmd_enable(',
    'static const char* meshesCmd_setdefault(',
    'static const char* meshesCmd_setpassphrase(',
    'const char* cmd_espnow_setname(',
)
# Present after the fix, absent on the pre-fix tree, which then still compiles
# and fails on the checks that name the defect.
HINT_HELPERS = (
    'static uint8_t espnowStartPassphraseSlot()',
    'static const char* meshLabelForPassphraseHint()',
    'static void formatNoPassphraseWarning(',
)
DELETE_HELPERS = (
    'static bool meshOtherEnabledExists(',
    'static int meshPairedPeerCount(',
    'static const char* meshDefaultInUseReply()',
)
DELETE = 'static const char* meshesCmd_delete('


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(a.component / 'test/host'))
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

    mesh_identity = definition(settings, 'struct MeshIdentity', 'System_Settings.h') + ';'

    have_hint = all(m in source for m in HINT_HELPERS)
    have_delete = DELETE in source and all(m in source for m in DELETE_HELPERS)

    # Source order matters: each piece only uses what is defined above it.
    order = [REQUIRED[0], REQUIRED[1], REQUIRED[2]]
    if have_hint:
        order += list(HINT_HELPERS)
    order += [REQUIRED[3], REQUIRED[4], REQUIRED[5]]
    if have_delete:
        order += list(DELETE_HELPERS)
    order.append(REQUIRED[6])
    if have_delete:
        order.append(DELETE)
    order += list(REQUIRED[7:])
    pieces = [definition(source, marker, 'System_ESPNow.cpp') for marker in order]

    # The subcommand is reachable and listed, and a start uses the shared helper.
    dispatcher = extract_block(source, 'const char* cmd_espnow_meshes(const String& argsInput)')
    if have_delete:
        assert 'if (sub == "delete")' in dispatcher and 'meshesCmd_delete(a.arg(1))' in dispatcher, \
            'espnowmeshes does not route "delete"'
        assert '[list|add|remove|delete|enable|setdefault|setpassphrase|rename]' in dispatcher, \
            'the usage line does not list delete'
    if have_hint:
        init = source[source.index('// Restore the encryption passphrase'):]
        init = init[:init.index('// Add broadcast peer')]
        assert 'gSettings.meshes[espnowStartPassphraseSlot()].passphrase' in init, \
            'ESP-NOW start does not use espnowStartPassphraseSlot()'
        assert 'formatNoPassphraseWarning(' in init and "espnowsetpassphrase primary" not in init, \
            'the start-up warning still names the mesh "primary"'

    harness = (HERE / 'espnow_mesh_slots_harness.cpp').read_text()
    for tag, value in (('// INSERT_MESH_IDENTITY', mesh_identity),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    print(f'delete subcommand: {"present" if have_delete else "absent"}; '
          f'passphrase hint helpers: {"present" if have_hint else "absent"}', flush=True)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-mesh-slots-') as tmp:
        unit, exe = Path(tmp) / 'espnow_mesh_slots.cpp', Path(tmp) / 'espnow_mesh_slots'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-function',
               f'-DHW1_HAVE_MESH_DELETE={1 if have_delete else 0}',
               f'-DHW1_HAVE_PASSPHRASE_HINT={1 if have_hint else 0}',
               '-I', str(a.component / 'test/host/web_batch_stubs'), str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode:
        print('FAIL espnow_mesh_slots: the mesh slot and device name checks reported failures', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_mesh_slots')


if __name__ == '__main__':
    main()
