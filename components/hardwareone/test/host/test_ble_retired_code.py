#!/usr/bin/env python3
"""Dead BLE code stays retired: BLE_IDF and the debugringbridge debug flag.

BLE_IDF.cpp/.h (an unbuilt ESP-IDF-native BLE stack behind
ENABLE_BLE_IDF_EXPERIMENTAL=0, with a second `struct BLEConnection`) are gone
from the component and its source list, and no board or deployment feature
header still sets the flag nothing reads. The `debugringbridge` flag lost its
last producer with the R1->G2 bridge; its X-macro row, command row, settings
row and gSettings field are gone, every other debug flag keeps its bit, and bit
230 is not reused (a saved `log` flags mask can still name it without turning
on another flag). The flag table is compiled on the host so its static_asserts
(row count, unique bits, banks, parents) hold, and the feature-gated row
subtraction in System_Debug.cpp matches the rows it gates. A debug file that
still carries ring.bridge loads: the reader looks keys up by registry row.

--source-dir points the suite at another copy of the component (the pre-fix
tree fails the BLE_IDF and debugringbridge cases).
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# Bits of the R1 ring family after the retirement; only RING_BRIDGE (230) left.
RING_BITS = {
    'RING': 224, 'RING_LIFECYCLE': 225, 'RING_SETUP': 226, 'RING_PROTOCOL': 227,
    'RING_TXN': 228, 'RING_HEALTH': 229, 'RING_DUMP': 231,
}
ROW_RE = re.compile(r'^\s*X\((\w+),\s*(\d+),\s*(\w+),\s*(\d+),', re.M)

failures = []


def check(ok, message):
    if not ok:
        failures.append(message)


def flag_rows(flags_h):
    start = flags_h.index('#define DBG_FLAG_LIST(X)')
    end = flags_h.index('#define', start + 10)
    return {sym: int(bit) for sym, bit, _bank, _parent in ROW_RE.findall(flags_h[start:end])}


def source_checks(component):
    repo = component.parents[1]
    # BLE_IDF
    for name in ('BLE_IDF.cpp', 'BLE_IDF.h'):
        check(not (component / name).exists(), f'{name} still exists in the component')
    cmake = (component / 'CMakeLists.txt').read_text()
    check(not re.search(r'^\s*BLE_IDF\.cpp\s*$', cmake, re.M),
          'components/hardwareone/CMakeLists.txt still compiles BLE_IDF.cpp')
    sources = sorted(component.glob('*.cpp')) + sorted(component.glob('*.h'))
    for path in sources:
        text = path.read_text(errors='replace')
        check('#include "BLE_IDF.h"' not in text, f'{path.name} includes BLE_IDF.h')
        check('ENABLE_BLE_IDF_EXPERIMENTAL' not in text,
              f'{path.name} still reads or sets ENABLE_BLE_IDF_EXPERIMENTAL')
    check('BLE_IDF' not in (component / 'BLE_Types.h').read_text(),
          'BLE_Types.h still names BLE_IDF.h as a consumer')
    headers = list((repo / 'boards').glob('*.h')) + list((repo / 'deployments').glob('*/boards/*/features.h'))
    check(headers, 'no board feature headers found; wrong --source-dir?')
    for path in headers:
        check('ENABLE_BLE_IDF_EXPERIMENTAL' not in path.read_text(),
              f'{path.relative_to(repo)} still sets ENABLE_BLE_IDF_EXPERIMENTAL, which nothing reads')

    # debugringbridge
    flags_h = (component / 'System_DebugFlags.h').read_text()
    rows = flag_rows(flags_h)
    check('RING_BRIDGE' not in rows, 'System_DebugFlags.h still has the RING_BRIDGE row')
    for sym, bit in RING_BITS.items():
        check(rows.get(sym) == bit, f'debug flag {sym} moved from bit {bit} to {rows.get(sym)}')
    check(all(bit != 230 for sym, bit in rows.items() if sym != 'RING_BRIDGE'),
          'retired bit 230 was reused by another debug flag')
    debug_cpp = (component / 'System_Debug.cpp').read_text()
    check('"debugringbridge"' not in debug_cpp, 'System_Debug.cpp still registers `debugringbridge`')
    check('DEBUG_RING_BRIDGEF' not in (component / 'System_Debug.h').read_text(),
          'System_Debug.h still defines DEBUG_RING_BRIDGEF')
    settings_cpp = (component / 'System_Settings.cpp').read_text()
    check('DBG_ROW(RING_BRIDGE)' not in settings_cpp, 'System_Settings.cpp still picks the RING_BRIDGE row')
    settings_h = (component / 'System_Settings.h').read_text()
    check(not re.search(r'\bbool\s+debugRingBridge\s*;', settings_h) and 'debugRingBridge(false)' not in settings_h,
          'System_Settings.h still carries the debugRingBridge field')

    # The non-G2 build subtracts exactly the generated rows the G2 span gates.
    table = debug_cpp[debug_cpp.index('const CommandEntry debugCommands[] = {'):]
    span_start = table.index('#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES')
    span = table[span_start:table.index('#endif', span_start)]
    gated = re.findall(r'\{\s*"(debugg2\w*|debugring\w*)"', span)
    sub = re.search(r'#if !\(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES\)\s*-\s*(\d+)', debug_cpp)
    check(sub is not None and int(sub.group(1)) == len(gated),
          f'the non-G2 row subtraction ({sub.group(1) if sub else "?"}) does not match the '
          f'{len(gated)} generated rows the G2 span gates')

    # Saved debug files: the reader looks keys up per registry row, so a key
    # with no row (ring.bridge) is ignored rather than rejected.
    reader = extract_block(settings_cpp, 'size_t readRegisteredSettings(JsonDocument& doc,')
    check('current[e->jsonKey]' in reader and 'for (size_t i = 0; i < mod->count; i++)' in reader,
          'readRegisteredSettings no longer walks registry rows; check that unknown keys still load')
    return flags_h


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstring>
@MASK@;
#define DEBUG_BIT(n) (((DebugFlagMask)1) << (n))
#include "System_DebugFlags.h"
int main() {
  int bad = 0;
  for (int i = 0; i < DBG_FLAG_COUNT; ++i) {
    if (std::strcmp(kDbgTag[i], "RING_BRIDGE") == 0) { std::puts("FAIL: RING_BRIDGE row compiled"); ++bad; }
    if (kDbgBit[i] == 230 && std::strcmp(kDbgTag[i], "RING_BRIDGE") != 0) {
      std::puts("FAIL: another compiled row claims retired bit 230");
      ++bad;
    }
  }
  if (DEBUG_RING_DUMP != DEBUG_BIT(231) || DEBUG_RING_HEALTH != DEBUG_BIT(229)) {
    std::puts("FAIL: a ring flag constant moved");
    ++bad;
  }
  return bad ? 1 : 0;
}
'''


def compile_check(component, flags_h, cxx, sanitize):
    mask = extract_block((component / 'System_Debug.h').read_text(), 'struct DebugFlagMask')
    with tempfile.TemporaryDirectory(prefix='hw1-ble-retired-') as tmp:
        work = Path(tmp)
        (work / 'System_DebugFlags.h').write_text(flags_h)
        unit = work / 'flags.cpp'
        unit.write_text(HARNESS.replace('@MASK@', mask))
        exe = work / 'flags'
        command = [cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(work), str(unit), '-o', str(exe)]
        if sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        built = subprocess.run(command, capture_output=True, text=True)
        if built.returncode:
            failures.append('the debug flag table does not compile on the host (static_assert?):\n'
                            + built.stderr[-1500:])
            return
        run = subprocess.run([str(exe)], capture_output=True, text=True)
        if run.returncode:
            failures.append('compiled debug flag table: ' + run.stdout.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source-dir', '--component', dest='source_dir', type=Path, default=COMPONENT)
    args = parser.parse_args()
    flags_h = source_checks(args.source_dir)
    compile_check(args.source_dir, flags_h, args.cxx, args.sanitize)
    for message in failures:
        print('FAIL', message, file=sys.stderr)
    if failures:
        sys.exit(1)
    print('retired BLE code: BLE_IDF and its flag are gone; debugringbridge is retired without moving '
          'another flag; the flag table compiles; the non-G2 subtraction matches; old debug files load')


if __name__ == '__main__':
    main()
