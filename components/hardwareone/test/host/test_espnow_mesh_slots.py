#!/usr/bin/env python3
"""Run the actual ESP-NOW mesh-slot commands and the device-name command.

Extracts the production Settings::MeshIdentity record and, from
System_ESPNow.cpp, meshFingerprintForLabel, the meshes settings writer and
reader, setEspNowPassphrase, initPrimaryMeshFromLegacySettings, the
start-passphrase and hint helpers, the espnowmeshes subcommands (add, remove,
delete, enable, setdefault, setpassphrase) with the delete guard's counters, and
cmd_espnow_setname, and runs them with the vendored ArduinoJson against host
doubles for settings writes, the key cache, stored identities, the device
registry, events and the ESP-NOW state.

Checks that a device's only mesh can be disabled and deleted (back to no mesh,
as on a new device), that delete frees a slot for 'add' and is refused while a
stored identity or a registry device still names it or while either list cannot
be read, that meshes keep their slot numbers across a settings save and load,
that the slot 0 bootstrap only runs on a device with no mesh at all, that
exactly one enabled mesh is the default, that the passphrase messages name the
mesh a start loads, and that the device name can be cleared while ESP-NOW is
stopped. --source-dir replays other production sources (for example the trees
before the fix) against this suite.
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
    'void espnowMeshesWriteJson(',
    'void espnowMeshesReadJson(',
    'static void setEspNowPassphrase(const String& passphrase)',
    'static void initPrimaryMeshFromLegacySettings()',
)
# Present after the fix, absent on the trees before it, which then still compile
# and fail on the checks that name the defect.
HINT_HELPERS = (
    'static uint8_t espnowStartPassphraseSlot()',
    'static const char* meshLabelForPassphraseHint()',
    'static void formatNoPassphraseWarning(',
)
ADD = (
    'static bool isValidMeshLabel(',
    'static int findFreeMeshSlot()',
    'static const char* meshesCmd_add(',
)
# Each is taken when the source has it, in this order.
DELETE_HELPERS = (
    'static bool meshOtherEnabledExists(',
    'static int meshPairedPeerCount(',
    'static int meshRegistryCountInJson(',
    'static int meshRegistryDeviceCount(',
    'static const char* meshDefaultInUseReply()',
)
REGISTRY_GUARD = 'static int meshRegistryDeviceCount('
REMOVE = 'static const char* meshesCmd_remove('
DELETE = 'static const char* meshesCmd_delete('
REST = (
    'static const char* meshesCmd_enable(',
    'static const char* meshesCmd_setdefault(',
    'static const char* meshesCmd_setpassphrase(',
    'const char* cmd_espnow_setname(',
)


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
    identity = read('System_ESPNow_Identity.cpp')

    def definition(text, marker, what):
        count = text.count(marker)
        if count != 1:
            sys.exit(f'FAIL extraction: {what} marker {marker!r} found {count} times, expected 1')
        return extract_block(text, marker)

    mesh_identity = definition(settings, 'struct MeshIdentity', 'System_Settings.h') + ';'

    have_hint = all(m in source for m in HINT_HELPERS)
    have_delete = DELETE in source
    have_guard = have_delete and REGISTRY_GUARD in source  # the delete guard that also reads the device registry

    # Source order matters: each piece only uses what is defined above it.
    order = list(BASE)
    if have_hint:
        order += list(HINT_HELPERS)
    order += list(ADD)
    order += [m for m in DELETE_HELPERS if m in source]
    order.append(REMOVE)
    if have_delete:
        order.append(DELETE)
    order += list(REST)
    pieces = [definition(source, marker, 'System_ESPNow.cpp') for marker in order]

    # What the harness cannot run is pinned in the source.
    init = ''
    if have_delete:
        dispatcher = extract_block(source, 'const char* cmd_espnow_meshes(const String& argsInput)')
        assert 'if (sub == "delete")' in dispatcher and 'meshesCmd_delete(a.arg(1))' in dispatcher, \
            'espnowmeshes does not route "delete"'
        assert '[list|add|remove|delete|enable|setdefault|setpassphrase|rename]' in dispatcher, \
            'the usage line does not list delete'
    if have_guard:
        # The registry file reader (a double in the harness): locked read, then the parser the harness runs.
        reader = definition(source, 'static int meshRegistryCountInFile(', 'System_ESPNow.cpp')
        assert 'if (!filesystemReady) return -1;' in reader, 'an unmounted filesystem must not read as "no devices"'
        assert 'FsLockGuard' in reader and 'ESPNOW_DEVICES_FILE' in reader, 'the registry file is not read under the lock'
        assert 'content.length() != expected' in reader, 'a short read must not be parsed'
        assert reader.rstrip().endswith('return meshRegistryCountInJson(content, slot);\n}'), \
            'the file reader does not hand its text to meshRegistryCountInJson'
        # The own address of the last start is recorded and never forgotten at a stop.
        cache = definition(source, 'static void setEspNowSelfMacCache(', 'System_ESPNow.cpp')
        assert 'gEspNowLastSelfMacValid = true;' in cache, 'the last-known own address is not recorded'
        assert source.count('gEspNowLastSelfMacValid = false') == 1, \
            'the last-known own address is cleared somewhere (only its initializer may say false)'
        # The identity cache says whether it was read this boot.
        load_all = definition(identity, 'uint8_t peerIdentityLoadAll()', 'System_ESPNow_Identity.cpp')
        assert load_all.index('gPeerIdentitiesLoaded = false;') < load_all.index('if (!filesystemReady) return 0;'), \
            'a load that cannot run must leave the identities marked as not read'
        assert load_all.count('gPeerIdentitiesLoaded = true;') == 2, \
            'the identities are marked read on other than the two complete paths (no directory, finished walk)'
    if have_hint:
        init = source[source.index('// Restore the encryption passphrase'):]
        init = init[:init.index('// Add broadcast peer')]
        assert 'gSettings.meshes[espnowStartPassphraseSlot()].passphrase' in init, \
            'ESP-NOW start does not use espnowStartPassphraseSlot()'
        assert 'formatNoPassphraseWarning(' in init and "espnowsetpassphrase primary" not in init, \
            'the start-up warning still names the mesh "primary"'
    if have_guard:
        assert 'gEspNow->passphrase = "";' in init and 'deriveKeyFromPassphrase(bootPw, gEspNow->derivedKey);' in init, \
            'a start without a passphrase keeps what the stopped instance held'

    harness = (HERE / 'espnow_mesh_slots_harness.cpp').read_text()
    for tag, value in (('// INSERT_MESH_IDENTITY', mesh_identity),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    print(f'delete subcommand: {"present" if have_delete else "absent"}; '
          f'registry guard: {"present" if have_guard else "absent"}; '
          f'passphrase hint helpers: {"present" if have_hint else "absent"}', flush=True)
    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-mesh-slots-') as tmp:
        unit, exe = Path(tmp) / 'espnow_mesh_slots.cpp', Path(tmp) / 'espnow_mesh_slots'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-function',
               '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
               '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
               f'-DHW1_HAVE_MESH_DELETE={1 if have_delete else 0}',
               f'-DHW1_HAVE_REGISTRY_GUARD={1 if have_guard else 0}',
               f'-DHW1_HAVE_PASSPHRASE_HINT={1 if have_hint else 0}',
               '-I', str(a.component / 'test/host/web_batch_stubs'), '-I', str(json_include),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode:
        print('FAIL espnow_mesh_slots: the mesh slot and device name checks reported failures', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_mesh_slots')


if __name__ == '__main__':
    main()
