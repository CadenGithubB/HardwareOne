#!/usr/bin/env python3
"""Run the production ESP-NOW V4 fragment reassembler against crafted plaintext fragments.

Extracts EspNowV4Header and the four opcode values it needs from
System_ESPNow_Wire.h, and from System_ESPNow.cpp the reassembly constants,
V4ReasmEntry, v4_reasm_reset/gc/find_or_alloc/release, v4_reasm_accept and
v4_dispatch_post_cleanup. The harness allocates the table exactly as init does
and feeds fragments as already-unwrapped plaintext (no session, ACK or radio).
It checks in-order and out-of-order completion with learned strides,
duplicates and GC; that a fragment whose type, count or index disagrees with
its entry is dropped; that an over-budget fragCount gets no entry; that a
completion larger than the buffer is refused; and that the cleanup paths are
safe when the table was never allocated. The harness runs as three
invocations so a sanitizer abort in one (on a pre-fix tree) does not hide the
others. Source contracts then check that v4_try_handle_incoming reaches the
table only through these helpers.

`--component <dir>` or `--source-dir <overlay>` replays another tree's
production sources (such as the saved base tree). For a tree whose reassembler
is still an inline block of v4_try_handle_incoming, that block is wrapped as a
function with the same contract, so the cases name the former defects.
extract_block is imported from the directory of this runner on purpose, not
from `<component>/test/host`: a component overlay supplies production sources
only, never a historical copy of the test code.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

# The inline reassembly block of a tree that predates v4_reasm_accept. The splice
# guard above it breaks its line after `&&`, so this marker matches only the block.
INLINE_BLOCK = 'if (h->fragCount > 1 && h->type != ESPNOW_V4_TYPE_ACK && h->type != ESPNOW_V4_TYPE_TEXT)'
FEED_FIXED = ('  const V4ReasmOutcome o = v4_reasm_accept(&ri, &h, p, n);\n'
              '  return {o == V4ReasmOutcome::Complete, p, n};')
# Inline block contract: `return true;` = frame handled and not complete;
# falling through = complete with payload/payloadLen pointing at the entry.
FEED_INLINE = ('  if (v4_reasm_accept_inline(&ri, &h, p, n)) return {false, nullptr, 0};\n'
               '  return {true, p, n};')
OPCODES = ('ACK', 'CMD_RESP', 'TEXT', 'FS_LIST_REPLY')
CONSTANTS = re.compile(r'^#define V4_(MAX_FRAGMENT_PAYLOAD|FRAG_MAX|REASM_MAX_MSG|REASM_MAX|REASM_TIMEOUT_MS)\b.*$',
                       re.MULTILINE)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path)
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')

    def read(name):
        overlay = a.source_dir / name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else a.component / name).read_text()

    source, wire = read('System_ESPNow.cpp'), read('System_ESPNow_Wire.h')
    opcodes = []
    for name in OPCODES:
        m = re.search(r'\bESPNOW_V4_TYPE_%s\s*=\s*(\d+)' % name, wire)
        if not m:
            sys.exit(f'FAIL: ESPNOW_V4_TYPE_{name} not found in System_ESPNow_Wire.h')
        opcodes.append(f'ESPNOW_V4_TYPE_{name} = {m.group(1)}')
    constants = [m.group(0) for m in CONSTANTS.finditer(source)]
    if len(constants) != 5:
        sys.exit(f'FAIL: expected five V4 reassembly #defines, found {len(constants)}')

    helpers = [extract_block(source, m) for m in (
        'static void v4_reasm_reset(V4ReasmEntry& e)', 'static void v4_reasm_gc(uint32_t nowMs)',
        'static V4ReasmEntry* v4_reasm_find_or_alloc(')]
    if 'static void v4_reasm_release(' in source:
        helpers.append(extract_block(source, 'static void v4_reasm_release('))
    helpers.append(extract_block(source, 'static void v4_dispatch_post_cleanup('))
    # The first match is the forward declaration; the definition is the last one.
    incoming = source[source.rindex('static bool v4_try_handle_incoming('):]
    incoming = extract_block(incoming, 'static bool v4_try_handle_incoming(')
    if 'static V4ReasmOutcome v4_reasm_accept(' in source:
        outcome = extract_block(source, 'enum class V4ReasmOutcome') + ';'
        accept = extract_block(source, 'static V4ReasmOutcome v4_reasm_accept(')
        feed = FEED_FIXED
    else:
        outcome = ''
        accept = ('static bool v4_reasm_accept_inline(const esp_now_recv_info* recv_info, const EspNowV4Header* h,\n'
                  '                                   const uint8_t*& payload, uint16_t& payloadLen) {\n'
                  + extract_block(incoming, INLINE_BLOCK) + '\n  return false;\n}')
        feed = FEED_INLINE

    harness = (HERE / 'espnow_reassembly_harness.cpp').read_text()
    for tag, value in [('// INSERT_OPCODES', 'enum : uint8_t { ' + ', '.join(opcodes) + ' };'),
                       ('// INSERT_WIRE_HEADER', extract_block(wire, 'struct __attribute__((packed)) EspNowV4Header') + ';'),
                       ('// INSERT_CONSTANTS', '\n'.join(constants)),
                       ('// INSERT_ENTRY', extract_block(source, 'struct V4ReasmEntry') + ';'),
                       ('// INSERT_OUTCOME', outcome),
                       ('// INSERT_HELPERS', '\n\n'.join(helpers)),
                       ('// INSERT_ACCEPT', accept),
                       ('// INSERT_FEED', feed)]:
        if harness.count(tag) != 1:
            sys.exit(f'FAIL: harness marker {tag} must appear exactly once')
        harness = harness.replace(tag, value)

    failures = []
    with tempfile.TemporaryDirectory(prefix='hw1-espnow-reasm-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=120)
        env = dict(os.environ)
        env.setdefault('UBSAN_OPTIONS', 'print_stacktrace=1')
        for args, what in (([], 'cases 1-7 and 9 (reassembly)'),
                           (['late-stride'], 'case 8 (completion wider than the buffer)'),
                           (['null-table'], 'case 10 (cleanup with no reassembly table)')):
            sys.stdout.flush()
            rc = subprocess.run([str(exe), *args], env=env, timeout=60).returncode
            if rc != 0:
                failures.append(f'{what}: harness exited with status {rc}')

    if 'v4_reasm_accept(recv_info, h, payload, payloadLen)' not in incoming:
        failures.append('v4_try_handle_incoming does not feed fragments through v4_reasm_accept')
    if 'gV4Reasm[' in incoming:
        failures.append('v4_try_handle_incoming indexes gV4Reasm directly instead of through the null-safe helpers')
    for line in failures:
        print(f'FAIL: {line}')
    if failures:
        sys.exit(1)
    print('ESP-NOW reassembly: all invocations and source contracts passed')


if __name__ == '__main__':
    main()
