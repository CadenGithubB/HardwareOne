#!/usr/bin/env python3
"""Compile actual filesystem permission and listing-view bodies with a fake roster.

The production normalizer, path rules, role resolver, owner policy, all canX APIs,
and listing view execute unchanged. Arduino strings, mutex/task ownership and
users.json reads are bounded host fakes; no filesystem or transport is accessed.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
DEFAULT_COMPONENT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, default=DEFAULT_COMPONENT)
    parser.add_argument('--source-dir', type=Path, help='Prepared source overlay for review before applying')
    args = parser.parse_args()
    sys.path.insert(0, str(args.component / 'test/host'))
    from test_web_batch_handlers import extract_block

    def read(name):
        path = args.source_dir / name if args.source_dir and (args.source_dir / name).exists() else args.component / name
        return path.read_text()

    source = read('System_Filesystem.cpp')
    internal = re.sub(r'^#include.*\n', '', read('System_Filesystem_Internal.h'), flags=re.M)
    header = read('System_Filesystem.h')
    pieces = [extract_block(header, 'enum FilePermission') + ';',
              extract_block(source, 'struct PathRule') + ';',
              extract_block(source, 'static const PathRule sPathRules[]') + ';']
    for marker in ('static const PathRule& lookupRule(', 'static bool hasSensitiveExtension(',
                   'static bool isImageFile(', 'bool normalizeFsPath(', 'enum class FsRole'):
        pieces.append(extract_block(source, marker) + (';' if marker.startswith('enum') else ''))
    for marker in ('static inline bool isUnrestrictedRole(', 'static FsRole resolveRole(',
                   'static uint8_t permsForRole(', 'static bool pathWithinScope(',
                   'static uint32_t transcriptAccountId(', 'static uint8_t transcriptPermissionMask(',
                   'static uint8_t permissionsForResolvedRole('):
        pieces.append(extract_block(source, marker))
    begin = source.index('LockedListingPermissions::LockedListingPermissions(')
    end = source.index('}  // namespace FsInternal', begin)
    pieces.append('namespace FsInternal {\n' + source[begin:end] + '\n}')
    for marker in ('static bool checkPerm(', 'bool canRead ', 'bool canEdit ', 'bool canDelete ',
                   'bool canRename ', 'bool canCreate ', 'bool canImport ', 'uint8_t getPermissions(',
                   'uint8_t getDirPerms('):
        pieces.append(extract_block(source, marker))
    harness_path = (args.source_dir / 'transcript_path_permissions_harness.cpp'
                    if args.source_dir and (args.source_dir / 'transcript_path_permissions_harness.cpp').exists()
                    else args.component / 'test/host/transcript_path_permissions_harness.cpp')
    harness = harness_path.read_text()
    policy = read('Transcript_PathPolicy.h').replace('#pragma once', '')
    for marker, value in (('// INSERT_POLICY', policy), ('// INSERT_INTERNAL', internal),
                          ('// INSERT_PRODUCTION', '\n\n'.join(pieces))):
        assert harness.count(marker) == 1
        harness = harness.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix='hw1-transcript-permissions-') as temp:
        unit, exe = Path(temp) / 'test.cpp', Path(temp) / 'test'
        unit.write_text(harness)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
