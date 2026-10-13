#!/usr/bin/env python3
"""Bond manifest and schema files go through one checked buffer, never a String.

An Arduino String stops at 65,535 bytes on builds without BOARD_HAS_PSRAM, and
the bond manifest (every command's name and help) is larger, so a String-built
manifest arrives cut short, is rejected, and bond sync re-sends it forever.

Source contracts: the manifest and schema senders and `bondshowmanifest`
serialize through serializeBondJson; the file-transfer receiver hands the
transfer buffer to the manifest and schema handlers without a String copy; the
CLI and web readers of a cached manifest use espnowReadCachedManifest, which
copies the file into a buffer under the filesystem lock instead of
readString().

Compiled: the production serializer, checked writer, manifest/schema
receivers, cache check, cached-manifest reader and console printer are
spliced into espnow_bond_json_harness.cpp with the vendored ArduinoJson and an
in-memory filesystem. A ~110 KB manifest is serialized, received, cached and
read back whole; a document over the receiver's 128 KB RAM limit, an allocation failure,
a short write, a failed read and malformed hashes are refused. The console
printer of a manifest dump waits for a modeled 64-line output queue instead of
dropping lines.

--source-dir <component-directory> reads production files from another tree
(for example the pre-fix base) while the harness always comes from this folder.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


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
    from test_web_batch_handlers import extract_block

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source = read('System_ESPNow.cpp')
    web = read('WebPage_ESPNow.cpp')
    failures = []

    def body(text, marker, where):
        if text.count(marker) != 1:
            raise SystemExit(f'{where}: {marker!r} found {text.count(marker)} times')
        return extract_block(text, marker)

    send_manifest = body(source, 'static void sendBondManifest(', 'System_ESPNow.cpp')
    send_schema = body(source, 'static void sendBondSchema(', 'System_ESPNow.cpp')
    show_manifest = body(source, 'const char* cmd_bond_showmanifest(', 'System_ESPNow.cpp')
    show_remote = body(source, 'const char* cmd_bond_showremotemanifest(', 'System_ESPNow.cpp')
    file_end = body(source, 'static void v4h_file_end(', 'System_ESPNow.cpp')
    web_manifest = body(web, 'static esp_err_t handleEspNowRemoteManifest(', 'WebPage_ESPNow.cpp')
    web_by_hash = body(web_manifest, 'if (strlen(fwHashParam) > 0)', 'handleEspNowRemoteManifest')

    for name, text in (('sendBondManifest', send_manifest), ('sendBondSchema', send_schema),
                       ('cmd_bond_showmanifest', show_manifest)):
        if 'serializeBondJson(' not in text or 'String manifest' in text or 'String schema' in text:
            failures.append(f'{name} serializes into a String (capped at 65,535 bytes) '
                            'instead of one measured buffer (serializeBondJson)')
    for needle in ('String manifestStr(', 'String schemaStr('):
        if needle in file_end:
            failures.append(f'v4h_file_end copies the received bond file into a String ({needle}...)')
    for name, text in (('cmd_bond_showremotemanifest', show_remote),
                       ('handleEspNowRemoteManifest', web_by_hash)):
        if 'readString()' in text or 'espnowReadCachedManifest(' not in text:
            failures.append(f'{name} loads the cached manifest with readString() instead of '
                            'espnowReadCachedManifest (buffer copy under the filesystem lock)')
    if 'FsLockGuard' in web_by_hash:
        failures.append('handleEspNowRemoteManifest takes the filesystem lock itself '
                        '(the owner reads the file; the response goes out unlocked)')
    if failures:
        for f in failures:
            print(f'espnow_bond_json: {f}', file=sys.stderr)
        return 1

    markers = ('static inline bool bondJsonEnvelopeOk(', 'static char* serializeBondJson(',
               'static bool writeBondJsonFileLocked(', 'static bool cacheManifestToLittleFS(',
               'static void processBondModeManifestResp(', 'static void processBondSchema(',
               'static bool manifestCachedFor(', 'size_t espnowReadCachedManifest(',
               'static void broadcastBondJsonChunks(')
    production = '\n'.join(body(source, m, 'System_ESPNow.cpp') for m in markers)

    harness = (HERE / 'espnow_bond_json_harness.cpp').read_text()
    tag = '// INSERT_PRODUCTION'
    if harness.count(tag) != 1:
        raise SystemExit(f'harness: {tag} must appear once')
    harness = harness.replace(tag, production + '\n')

    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-bond-json-') as tmp:
        unit, exe = Path(tmp) / 'espnow_bond_json.cpp', Path(tmp) / 'espnow_bond_json'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-pedantic', '-Werror',
               '-Wno-unused-parameter',  # processBondModeManifestResp does not read srcMac
               '-I', str(json_include),
               str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=180)
        result = subprocess.run([str(exe)], timeout=120)
        if result.returncode:
            print('espnow_bond_json: FAILED', file=sys.stderr)
            return 1
    print('espnow_bond_json: OK')
    return 0


if __name__ == '__main__':
    sys.exit(main())
