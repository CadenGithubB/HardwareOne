#!/usr/bin/env python3
"""Run the actual ESP-NOW mesh-slot commands and the device-name command.

Extracts the production Settings::MeshIdentity record and, from
System_ESPNow.cpp, meshFingerprintForLabel, the meshes settings writer and
reader, setEspNowPassphrase, initPrimaryMeshFromLegacySettings, the
start-passphrase and hint helpers, the helpers that keep a running instance on
the passphrase a start would load, the espnowmeshes subcommands (add, remove,
delete, enable, setdefault, setpassphrase, listjson) with the delete guard's
counters, and cmd_espnow_setname, and runs them with the vendored ArduinoJson against host
doubles for settings writes, the key cache, stored identities, the device
registry, events and the ESP-NOW state.

Checks that a device's only mesh can be disabled and deleted (back to no mesh,
as on a new device), that delete frees a slot for 'add' and is refused while a
stored identity or a registry device still names it or while either list cannot
be read, that meshes keep their slot numbers across a settings save and load,
that the slot 0 bootstrap only runs on a device with no mesh at all, that
exactly one enabled mesh is the default, that the passphrase messages name the
mesh a start loads, that a running instance always holds the passphrase a start
would load (a new passphrase on that mesh, setdefault, enable, disable and delete
apply at once) and every reply says what is in use, that a disabled mesh keys
nothing (slot 0's passphrase counts only while its mesh is enabled), that an
instance whose stop is still pending is left holding no passphrase, that
clearing slot 0's passphrase follows the rule of 'espnowmeshes remove' (refused
while it is the default beside another enabled mesh, else disabled and no longer
the default), that slot 0's passphrase is stored before the first start, and that
the device name can be cleared while ESP-NOW is stopped. Source checks pin the
start's use of the loader and the web page's mesh table, passphrase forms and
action panel (escaped labels, Clear passphrase greyed out where the firmware
refuses it), which are not runnable here. --source-dir replays other production sources (for example the trees
before the fix) against this suite.
"""
import argparse
import os
import re
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
# and fail on the checks that name the defect. The start slot returns -1 for "none"
# since a disabled slot 0 stopped keying the device (int8_t); before, it always
# named a slot (uint8_t). The harness compares it as an int, so both compile.
START_SLOT = ('static int8_t espnowStartPassphraseSlot()', 'static uint8_t espnowStartPassphraseSlot()')
HINT_REST = (
    'static const char* meshLabelForPassphraseHint()',
    'static void formatNoPassphraseWarning(',
)
# The running instance follows what a start would load. Present after the fix.
# espnowStartPassphraseSource went away when the start slot learned to say "none";
# it is taken when the source has it.
LIVE_SOURCE = 'static int8_t espnowStartPassphraseSource()'
LIVE_LOADER = (
    'static void espnowLoadLivePassphrase()',
    'static void formatLivePassphraseNote(',
    'static int8_t espnowPassphraseInUse()',
    'static void appendLivePassphraseNote()',
)
LISTJSON = 'static const char* meshesCmd_listjson()'
PAIRING = 'static uint8_t meshSlotForPairing('
PARSE_MESH_ARG = 'static uint8_t parseMeshArgOrDefault('
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
    'static const char* meshesCmd_rename(',
    'static int meshSlotByLabel(',
    'const char* cmd_espnow_setname(',
)
LIVE_SLOT = 'static int8_t gEspNowLivePassphraseSlot = -1;'



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

    start_slot = next((m for m in START_SLOT if m in source), START_SLOT[0])
    hint_helpers = (start_slot,) + HINT_REST
    have_hint = all(m in source for m in hint_helpers)
    have_delete = DELETE in source
    have_guard = have_delete and REGISTRY_GUARD in source  # the delete guard that also reads the device registry
    have_pairing = PAIRING in source
    have_live_slot = LIVE_SLOT in source  # the instance records which slot its passphrase came from
    have_loader = have_hint and all(m in source for m in LIVE_LOADER)

    # Source order matters: each piece only uses what is defined above it.
    order = list(BASE)
    if have_hint:
        order += list(hint_helpers)
    if have_loader:
        order += ([LIVE_SOURCE] if LIVE_SOURCE in source else []) + list(LIVE_LOADER)
    order.append(LISTJSON)
    order.append(PARSE_MESH_ARG)
    if have_pairing:
        order.append(PAIRING)
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
        assert load_all.count('gPeerIdentitiesLoaded = true;') == (1 if have_pairing else 2), \
            'the identities are marked read on a path that did not read them'
    if have_hint:
        init = source[source.index('// Restore the encryption passphrase'):]
        init = init[:init.index('// Add broadcast peer')]
        assert ('espnowStartPassphraseSlot()' in init or 'espnowLoadLivePassphrase();' in init) and \
            'gSettings.meshes[0].passphrase' not in init, \
            'ESP-NOW start does not use espnowStartPassphraseSlot()'
        assert 'formatNoPassphraseWarning(' in init and "espnowsetpassphrase primary" not in init, \
            'the start-up warning still names the mesh "primary"'
    if have_loader:
        # The start loads through the same helper the mesh commands use.
        assert 'espnowLoadLivePassphrase();' in init and 'gEspNow->passphrase =' not in init, \
            'the start does not load its passphrase through espnowLoadLivePassphrase()'
        assert 'formatNoPassphraseWarning(' in init[init.index('} else {'):], \
            'a start without a passphrase does not warn'
        # The loader does nothing for an instance that is not marked initialized, so
        # initEspNow must set the flag before it loads (the harness's start does the same).
        start = definition(source, 'static bool initEspNow()', 'System_ESPNow.cpp')
        assert start.count('gEspNow->initialized = true;') == 1 and start.count('espnowLoadLivePassphrase();') == 1 and \
            start.index('gEspNow->initialized = true;') < start.index('espnowLoadLivePassphrase();'), \
            'initEspNow loads the passphrase before it marks the instance initialized'
    if have_guard and not have_loader:
        # The branch of a start that found no passphrase: it clears the name and the key.
        no_passphrase = init[init.index('} else {'):]
        assert 'gEspNow->passphrase = "";' in no_passphrase and \
            'deriveKeyFromPassphrase(bootPw, gEspNow->derivedKey);' in no_passphrase, \
            'a start without a passphrase keeps what the stopped instance held'
    if have_live_slot:
        # A start records the slot it loaded the passphrase from, a stop forgets it, and a
        # registry save that fails leaves the "file is behind" mark (the harness doubles all three).
        assert have_loader or 'gEspNowLivePassphraseSlot = bootPw.length() > 0 ? (int8_t)bootPwSlot : -1;' in init, \
            'the start does not record which slot its passphrase came from'
        stop = definition(source, 'static bool deinitEspNow(bool publishOffEvent)', 'System_ESPNow.cpp')
        assert 'gEspNowLivePassphraseSlot = -1;' in stop, 'a stop keeps the live passphrase slot'
        saver = definition(source, 'static bool saveEspNowDevices()', 'System_ESPNow.cpp')
        assert '~Outcome() { sEspNowDevicesFileBehind = !written; }' in saver and \
            saver.count('outcome.written = true;') == 1 and \
            saver.rstrip().endswith('outcome.written = true;\n  return true;\n}'), \
            'a registry save that does not reach the file is not marked'
        assert saver.index('} outcome;') < saver.index('if (!filesystemReady) return false;'), \
            'a save refused for an unmounted filesystem is not marked'
        forget = definition(identity, 'bool peerIdentityForget(const uint8_t mac[6])', 'System_ESPNow_Identity.cpp')
        assert 'gPeerIdentitiesLoaded = false;' in forget, \
            'an identity file that could not be removed leaves the cache marked complete'
    if have_pairing:
        # Both pair commands take their slot from the helper; neither falls back to slot 0 itself.
        assert source.count('meshSlotForPairing(a.arg(2))') == 2, 'a pair command does not use meshSlotForPairing'
        assert 'if (meshId == 0xFE) meshId = 0' not in source, 'a pair command still defaults to slot 0'
        # The slot 0 passphrase setter no longer creates a mesh.
        setter = definition(source, 'static void setEspNowPassphrase(const String& passphrase)', 'System_ESPNow.cpp')
        assert 'm0.label = "primary"' not in setter, 'setEspNowPassphrase still creates "primary"'
        # An identity that is stored but did not load leaves the cache marked incomplete.
        assert 'gPeerIdentitiesLoaded = notLoaded == 0;' in load_all and 'VFS::existsGuarded(path, auth)' in load_all, \
            'a stored identity that was not loaded does not mark the cache incomplete'

    # The web page's mesh table and passphrase forms (not runnable here, so pinned in
    # the source): the table marks the mesh whose passphrase the device uses, from
    # listjson's passphraseSlot; the clear dialog does not claim that all encrypted
    # traffic stops (paired devices keep their keys and sessions), but for slot 0,
    # whose mesh the firmware disables on a clear, it says the mesh is disabled and
    # its devices cannot exchange frames, and for slots 1..3 that heartbeats and time
    # sync stop getting through; the passphrase forms show the firmware's answer,
    # which says what applies now. These are counted, not fatal, so a replay of an
    # older page still runs the harness.
    page = read('WebPage_ESPNow.h')
    page_failures = []
    PAGE_CHECKS = 7
    table = page[page.find('function renderMeshesTable('):page.find('function updatePairChooser(')]
    if 'data.passphraseSlot' not in table or 'data.running' not in table or '(device passphrase)' not in table:
        page_failures.append('the mesh table does not mark the mesh whose passphrase the device uses')
    if 'Encrypted communication will fail until a new passphrase is set' in page:
        page_failures.append('the clear-passphrase dialog says encrypted communication stops')
    clear_button = page[page.find('btn btn-mesh-clearpass'):]
    clear_button = clear_button[:clear_button.find('</button>')]
    clear_handler = page[page.find("t.classList.contains('btn-mesh-clearpass')"):page.find("'MESH_PASS_CLEAR')")]
    if 'data-slot=' not in clear_button or "String(slot) === '0'" not in clear_handler or \
            'This also disables the mesh' not in clear_handler or 'cannot exchange frames' not in clear_handler or \
            'heartbeats and time sync' not in clear_handler:
        page_failures.append('the clear-passphrase dialog does not say that slot 0\'s mesh is disabled '
                             '(frames stop) and that heartbeats stop on the others')
    for tag in ("'MESH_PASS')", "'MESH_PASS_CLEAR')"):
        at = page.find(tag)
        if at < 0 or 'alert(text)' not in page[at:at + 120]:
            page_failures.append(f'the passphrase form {tag[1:-2]} does not show the firmware\'s answer')
    # The action panel and its subforms put the label into attributes escaped: a
    # label may hold '&' and ';', and the click handler reads it back from dataset,
    # which decodes entities, so a raw label could name another mesh.
    panel = page[page.find('function renderActionPanel('):page.find('function findMesh(')]
    pass_form = page[page.find('function showPassphraseSubform('):page.find('function showRenameSubform(')]
    rename_form = page[page.find('function showRenameSubform('):page.find('function clearSubform(')]
    if "\"' + label + '\"" in panel or 'escHtml(label)' not in panel or \
            "data-label=\"' + escHtml(label) + '\"" not in pass_form or \
            "data-oldlabel=\"' + escHtml(label) + '\"" not in rename_form:
        page_failures.append('the action panel or its subforms write the raw mesh label into an attribute')
    # Clearing slot 0's passphrase disables its mesh, so the page offers it under the
    # rule Disable follows: not while slot 0 is the default and another mesh is enabled.
    if "String(slot) === '0' && isDefault && otherEnabled" not in panel or \
            not re.search(r'<button class="btn" disabled[^>]*>Clear passphrase</button>', panel):
        page_failures.append('the action panel offers Clear passphrase on the default slot 0 mesh '
                             'while another mesh is enabled')
    for failure in page_failures:
        print(f'FAIL web page: {failure}', flush=True)

    harness = (HERE / 'espnow_mesh_slots_harness.cpp').read_text()
    for tag, value in (('// INSERT_MESH_IDENTITY', mesh_identity),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    print(f'delete subcommand: {"present" if have_delete else "absent"}; '
          f'registry guard: {"present" if have_guard else "absent"}; '
          f'pairing slot helper: {"present" if have_pairing else "absent"}; '
          f'passphrase hint helpers: {"present" if have_hint else "absent"}; '
          f'live passphrase loader: {"present" if have_loader else "absent"}', flush=True)
    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-mesh-slots-') as tmp:
        unit, exe = Path(tmp) / 'espnow_mesh_slots.cpp', Path(tmp) / 'espnow_mesh_slots'
        unit.write_text(harness)
        # parseMeshArgOrDefault reads a slot number with String::toInt, which the
        # shared stub leaves to each test (others add their own the same way).
        stub = (a.component / 'test/host/web_batch_stubs/Arduino.h').read_text()
        anchor = '  char operator[]'
        assert stub.count(anchor) == 1 and 'toInt' not in stub, 'String stub changed; adjust the toInt insertion'
        stub = stub.replace(anchor, '  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }\n' + anchor)
        stub = stub.replace('#include <string>', '#include <cstdlib>\n#include <string>', 1)
        (Path(tmp) / 'Arduino.h').write_text(stub)
        shutil.copy(a.component / 'System_CommandReply.h', Path(tmp) / 'System_CommandReply.h')
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-unused-function',
               '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
               '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
               f'-DHW1_HAVE_MESH_DELETE={1 if have_delete else 0}',
               f'-DHW1_HAVE_REGISTRY_GUARD={1 if have_guard else 0}',
               f'-DHW1_HAVE_PAIRING_SLOT={1 if have_pairing else 0}',
               f'-DHW1_HAVE_LIVE_SLOT={1 if have_live_slot else 0}',
               f'-DHW1_HAVE_PASSPHRASE_HINT={1 if have_hint else 0}',
               f'-DHW1_HAVE_LIVE_LOADER={1 if have_loader else 0}',
               '-I', tmp, '-I', str(json_include),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        result = subprocess.run([str(exe)], timeout=60)
    print(f'web page source checks: {PAGE_CHECKS - len(page_failures)} of {PAGE_CHECKS} passed', flush=True)
    if result.returncode or page_failures:
        print('FAIL espnow_mesh_slots: the mesh slot and device name checks reported failures', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_mesh_slots')


if __name__ == '__main__':
    main()
