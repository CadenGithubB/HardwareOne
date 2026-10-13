#!/usr/bin/env python3
"""Run the actual ESP-NOW metadata text policy, receive path and CLI setter.

Extracts the production metadata types (EspNowInlineText, MeshPeerMeta,
V4PayloadMetadata, V4RxCtx) and functions (the metadata text policy when the
source has it, normalizeMetadataWireText, v4h_metadata_resp_push, processMetadata,
metaGetSet) and runs them against host doubles for the registry, mesh cache,
settings writes, events and logging. Checks that hostile peer text is replaced
before it reaches the mesh cache or the persisted registry, that the local
setters reject control bytes and < > " ' & \\ before any write, and that clean
ASCII/UTF-8 values are stored unchanged. --source-dir replays other production
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
    pieces += [definition(source, marker, 'System_ESPNow.cpp') for marker in FUNCTION_MARKERS]

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
               '-I', str(a.component / 'test/host/web_batch_stubs'), str(unit), '-o', str(exe)]
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
