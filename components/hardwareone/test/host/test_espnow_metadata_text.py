#!/usr/bin/env python3
"""Run the actual ESP-NOW metadata text policy, receive path and CLI setter.

Extracts the production metadata types (EspNowInlineText, MeshPeerMeta,
V4PayloadMetadata, V4RxCtx) and functions (the metadata text policy when the
source has it, normalizeMetadataWireText, the per-sender metadata inbox with
v4h_metadata_req, v4h_metadata_resp_push and metadataInboxHandleFrame,
processMetadata with the room and tag membership functions it calls,
metaGetSet; the rules header System_ESPNow_GroupCore.h is compiled as is) and
runs them against host doubles for the registry, mesh cache, message history,
settings writes, events and logging. Checks that hostile peer text is replaced
before it reaches the mesh cache or the persisted registry, that the local
setters reject control bytes and < > " ' & \\ before any write, that clean
ASCII/UTF-8 values are stored unchanged, and that the metadata frames of
several peers received in one pass of the task are all handled (a frame is how
a peer leaves a room or a tag), each sender's latest one, with a history line
except for the answer to a refresh this device asked for by itself. --source-dir replays other production
sources (for example the pre-fix tree) against this suite.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

# Optional policy helpers: present after the fix, absent on the pre-fix tree, which
# then still compiles and fails on the behavioral checks that name the defect.
POLICY_MARKERS = (
    'static bool metadataTextByteAllowed(',
    'static bool metadataTextAllowed(',
    'static size_t scrubMetadataText(',
)
NORMALIZE_MARKERS = (
    'static size_t normalizeMetadataWireText(',
    'static void normalizeMetadataWireText(',
)
FUNCTION_MARKERS = (
    'static void v4h_metadata_resp_push(',
    'static void processMetadata(',
    'static const char* metaGetSet(',
)
# processMetadata hands a paired peer's room and tags to the announcement part
# of room and tag membership (test_espnow_group_membership.py), and received
# frames wait in a per-sender inbox. Absent on a tree before them, which keeps
# one shared buffer in the state.
GROUP_MARKERS = (
    'static espnowgroup::Membership groupMembershipOf(',
    'static void groupMembershipStore(',
    'static EspNowDevice* groupRecordOf(',
    'static bool groupSnapshotOf(',
)
ANNOUNCE_REGION = ('// ---- Room and tag membership: what a peer announces -----',
                   '// ---- Room and tag membership: acceptance -----')
INBOX_REGION = ('// ---- Metadata work waiting for espnow_task -----',
                'static void v4h_metadata_req(const V4RxCtx& ctx) {')
INBOX_MARKERS = (
    'static void v4h_metadata_req(',
    'static bool metadataInboxHandleFrame(',
)
GROUP_INCLUDES = '#include "System_ESPNow_GroupCore.h"\n#include "System_MacText.h"'


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
    header = read('System_ESPNow.h')
    wire = read('System_ESPNow_Wire.h')
    rxctx = read('System_ESPNow_RxCtx.h')

    def definition(text, marker, what):
        count = text.count(marker)
        if count != 1:
            sys.exit(f'FAIL extraction: {what} marker {marker!r} found {count} times, expected 1')
        return extract_block(text, marker)

    types = [
        'template <size_t Capacity>\n' + definition(header, 'class EspNowInlineText', 'System_ESPNow.h') + ';',
        definition(header, 'struct MeshPeerMeta', 'System_ESPNow.h') + ';',
        definition(wire, 'struct __attribute__((packed)) V4PayloadMetadata', 'System_ESPNow_Wire.h') + ';',
        definition(rxctx, 'struct V4RxCtx', 'System_ESPNow_RxCtx.h') + ';',
    ]

    pieces = []
    have_policy = all(marker in source for marker in POLICY_MARKERS)
    if have_policy:
        pieces += [definition(source, marker, 'System_ESPNow.cpp') for marker in POLICY_MARKERS]
    normalize = next((m for m in NORMALIZE_MARKERS if m in source), None)
    if normalize is None:
        sys.exit('FAIL extraction: no normalizeMetadataWireText definition in System_ESPNow.cpp')
    pieces.append(definition(source, normalize, 'System_ESPNow.cpp'))
    def region(first, after):
        if source.count(first) != 1 or source.count(after) != 1:
            sys.exit(f'FAIL extraction: region {first!r} .. {after!r} is not unique in System_ESPNow.cpp')
        return source[source.index(first):source.index(after)]

    have_inbox = INBOX_REGION[0] in source
    if have_inbox:
        pieces.append(GROUP_INCLUDES)
        pieces += [definition(source, marker, 'System_ESPNow.cpp') for marker in GROUP_MARKERS]
        pieces.append(region(*ANNOUNCE_REGION))
        pieces.append(region(*INBOX_REGION))
    pieces += [definition(source, marker, 'System_ESPNow.cpp') for marker in FUNCTION_MARKERS]
    if have_inbox:
        pieces += [definition(source, marker, 'System_ESPNow.cpp') for marker in INBOX_MARKERS]

    # The registry saver consults the load status before it writes anything, and
    # the loader records that status before its own migration save.
    saver = source[source.index('static bool saveEspNowDevices() {'):]
    gate = saver.find('if (!ConfigLoad::savable(sEspNowDevicesLoad))')
    assert 0 <= gate < saver.index('ESPNOW_DEVICES_TEMP_FILE'), 'device registry save is not gated on the load status'
    loader = source[source.index('static void loadEspNowDevices() {'):]
    partial = loader.find('sEspNowDevicesLoad = ConfigLoad::Status::Partial;')
    assert 0 <= partial < loader.index('if (migrationBackupNeeded)'), 'load status is set after the migration save'

    harness = (HERE / 'espnow_metadata_text_harness.cpp').read_text()
    for tag, value in (('// INSERT_TYPES', '\n\n'.join(types)),
                       ('// INSERT_PRODUCTION', '\n\n'.join(pieces))):
        assert harness.count(tag) == 1, tag
        harness = harness.replace(tag, value)

    print(f'metadata text policy helpers: {"present" if have_policy else "absent (policy table case skipped)"}', flush=True)
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-metadata-text-') as tmp:
        unit, exe = Path(tmp) / 'espnow_metadata_text.cpp', Path(tmp) / 'espnow_metadata_text'
        unit.write_text(harness)
        # The log doubles keep printf format checking; -Wformat-pedantic is off
        # because production logs typed pointers with %p (fine on the target).
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', '-Wno-format-pedantic',
               f'-DHW1_HAVE_METADATA_POLICY={1 if have_policy else 0}',
               f'-DHW1_HAVE_METADATA_INBOX={1 if have_inbox else 0}',
               '-I', str(a.component / 'test/host/web_batch_stubs'), '-I', str(a.component),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=60)
    if result.returncode:
        print('FAIL espnow_metadata_text: the metadata text checks reported failures', file=sys.stderr)
        sys.exit(1)
    print('PASS espnow_metadata_text')


if __name__ == '__main__':
    main()
