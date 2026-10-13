#!/usr/bin/env python3
"""Actual QuickNote store (create/list/read/delete) against a fake guarded VFS.

QuickNote_Store.cpp, QuickNote_Policy.h, Transcript_PathPolicy.h and the
production filesystem path normalizer compile unchanged. The guarded VFS is an
in-memory tree whose permission decision is the production transcript owner
mask (TranscriptPathPolicy::permissionMask) with the role tiers
System_Filesystem.cpp layers on it (system/superadmin unrestricted, guest
read-only, scope confinement); the real permission pipeline itself, notes
folder included, is test_transcript_path_permissions.py.

--component points the production sources at another tree (the base run).
A tree whose store has no QuickNoteStore::sweep (before the unfinished-file
sweep existed) gets a no-op one, which is what its service did at start:
nothing.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

from test_espsr_runtime import function

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def clean(text):
    return re.sub(r'^#include.*\n|^#pragma once\n', '', text, flags=re.M)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--component', type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()
    policies = '\n'.join(clean((component / name).read_text()) for name in
                         ('Transcript_PathPolicy.h', 'Transcription_UI_Policy.h', 'QuickNote_Policy.h'))
    normalizer = function((component / 'System_Filesystem.cpp').read_text(), 'bool normalizeFsPath(')
    store = (clean((component / 'QuickNote_Store.h').read_text()) +
             clean((component / 'QuickNote_Store.cpp').read_text()))
    if 'bool sweep(uint32_t* removed, const char** why) {' not in store:
        print('QuickNote_Store.cpp has no QuickNoteStore::sweep: the service start removes nothing', flush=True)
        store += ('\nnamespace QuickNoteStore {\n'
                  'bool sweep(uint32_t* removed, const char** why) {\n'
                  '  if (removed) *removed = 0;\n'
                  '  if (why) *why = nullptr;\n'
                  '  return true;\n'
                  '}\n'
                  '}  // namespace QuickNoteStore\n')
    harness = (HERE / 'quicknote_store_harness.cpp').read_text()
    for marker, value in (('// INSERT_POLICIES', policies), ('// INSERT_NORMALIZER', normalizer),
                          ('// INSERT_STORE', store)):
        assert harness.count(marker) == 1, marker
        harness = harness.replace(marker, value)
    with tempfile.TemporaryDirectory(prefix='hw1-quicknote-store-') as tmp:
        tmp = Path(tmp)
        # Same bounded host String as the HTTP suite, extended only for the
        # production normalizer's reserve/character append surface.
        arduino = (HERE / 'web_batch_stubs/Arduino.h').read_text().replace(
            '  String& operator+=(const String& other)',
            '  void reserve(size_t n) { text_.reserve(n); }\n'
            '  String& operator+=(char c) { text_ += c; return *this; }\n'
            '  String& operator+=(const String& other)')
        (tmp / 'Arduino.h').write_text(arduino)
        unit, exe = tmp / 'test.cpp', tmp / 'test'
        unit.write_text(harness)
        # The policy header is pasted in: constants only the service uses
        # (kWhenMax) are unused here, as in test_quicknote_service.py.
        cmd = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
               '-Wno-unused-const-variable', '-I', str(tmp), str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=120)


if __name__ == '__main__':
    main()
