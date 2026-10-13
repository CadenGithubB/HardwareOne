#!/usr/bin/env python3
"""Run the actual ESP-NOW metadata setters, payload builder, sender and push tick.

Extracts metaGetSet (with the metadata text policy when the source has it),
cmd_espnow_stationary, cmd_espnow_setname, buildMetadataPayload, sendMetadata,
the metadata tracking block and metadataPushTick from System_ESPNow.cpp, plus the
real SessionState, EspNowEventCategory and V4PayloadMetadata definitions, and
compiles them beside session-table, subscription, transport, settings and clock
doubles. The harness checks that a local metadata change is pushed (type 55)
once per ACTIVE, subscribed session after the settle window, that a burst of
edits coalesces, that the deadline is millis()-wrap safe, that a failed send is
not retried and that the request/response path is unchanged. No hardware or
radio is accessed.

--component <hardwareone dir> reads every production file from another tree;
--source-dir <dir> overlays single files. A tree without metadataPushTick
compiles against a no-op tick so the run fails on behavior, not on extraction.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

ESPNOW_MARKERS = (
    'static void buildMetadataPayload(',
    'static void sendMetadata(',
    'static const char* metaGetSet(',
    '\nconst char* cmd_espnow_stationary(',
    '\nconst char* cmd_espnow_setname(',
    '// Metadata transmission tracking',
    '// Master/Backup heartbeat tracking',
)
POLICY_MARKERS = ('static bool metadataTextByteAllowed(', 'static bool metadataTextAllowed(')
TICK_MARKER = 'static int metadataPushTick('
NO_TICK = ('static const uint32_t METADATA_PUSH_SETTLE_MS = 1500;  // source has no push tick\n'
           'static int metadataPushTick(uint32_t) { return 0; }')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(HERE))
    from test_web_batch_handlers import extract_block, require

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow.cpp')
    sessions = read('System_ESPNow_Sessions.h')
    identity = read('System_ESPNow_Identity.h')
    wire = read('System_ESPNow_Wire.h')

    for marker in ESPNOW_MARKERS:
        require(source.count(marker) == 1, f'marker must be unique: {marker!r} ({source.count(marker)} hits)')
    for text, marker in ((sessions, 'enum SessionStateLifecycle'), (sessions, 'struct SessionState'),
                         (identity, 'enum EspNowEventCategory'),
                         (wire, 'struct __attribute__((packed)) V4PayloadMetadata')):
        require(text.count(marker) == 1, f'marker must be unique: {marker!r} ({text.count(marker)} hits)')

    tracking = source[source.index('// Metadata transmission tracking'):
                      source.index('// Master/Backup heartbeat tracking')]
    policy = [m for m in POLICY_MARKERS if source.count(m) == 1]
    production = '\n\n'.join(extract_block(source, m) for m in (
        *policy, 'static void buildMetadataPayload(', 'static void sendMetadata(',
        'static const char* metaGetSet(', '\nconst char* cmd_espnow_stationary(',
        '\nconst char* cmd_espnow_setname('))
    tick = extract_block(source, TICK_MARKER) if source.count(TICK_MARKER) == 1 else NO_TICK

    harness = (HERE / 'espnow_metadata_push_harness.cpp').read_text()
    for tag, value in (('// INSERT_WIRE', extract_block(wire, 'struct __attribute__((packed)) V4PayloadMetadata') + ';'),
                       ('// INSERT_SESSIONS', extract_block(sessions, 'enum SessionStateLifecycle') + ';\n'
                                              + extract_block(sessions, 'struct SessionState') + ';'),
                       ('// INSERT_IDENTITY', extract_block(identity, 'enum EspNowEventCategory') + ';'),
                       ('// INSERT_TRACKING', tracking),
                       ('// INSERT_PRODUCTION', production),
                       ('// INSERT_TICK', tick)):
        require(harness.count(tag) == 1, f'missing/duplicate harness tag: {tag}')
        harness = harness.replace(tag, value)

    with tempfile.TemporaryDirectory(prefix='hw1-espnow-metadata-push-') as tmp:
        unit, exe = Path(tmp) / 'espnow_metadata_push.cpp', Path(tmp) / 'espnow_metadata_push'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-I', str(HERE / 'web_batch_stubs'), str(unit), '-o', str(exe)]
        if len(policy) == len(POLICY_MARKERS):
            cmd.insert(1, '-DHW1_HAVE_METADATA_POLICY')
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=60)
        require(result.returncode == 0, f'espnow_metadata_push harness failed (exit {result.returncode})')
    print('espnow_metadata_push: ok')


if __name__ == '__main__':
    main()
