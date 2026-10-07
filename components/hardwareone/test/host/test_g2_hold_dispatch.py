#!/usr/bin/env python3
"""G2 long-press (hold) dispatch: the production hold section, run.

Splices the whole hold section of G2_Glasses.cpp (pairing, gates, watchdog
activity, job queueing, g2_tap_disp delivery, page side table, global action,
events, implicit aborts, expiry and `g2hold sim`), and the exit tail of
handleDevEvent's SysEvent branch (SYSTEM_EXIT's own-echo/terminal split and
the implicit hold releases), into g2_hold_dispatch_harness.cpp, which fakes the
platform and the rest of the G2 client, and runs it.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
START = ('// -----------------------------------------------------------------------------\n'
         '// Long-press ("hold") input: decode, pair, record and dispatch\n')
END = '// Called by g2_tap_disp after g2_ctrl_owner sees and deduplicates a ListEvent\n'
TAIL_START = 'bool ownShutdownEcho = false;'
TAIL_END = 'if ((etype == 4 || etype == 5 || etype == 6 || etype == 7) &&'


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--sanitize', action='store_true')
    a = p.parse_args()
    c = a.component.resolve()
    sys.path.insert(0, str(c / 'test/host'))
    from test_web_batch_handlers import extract_block

    source = (c / 'G2_Glasses.cpp').read_text()
    assert source.count(START) == 1 and source.count(END) == 1
    begin = source.index(START)
    code = source[begin:source.index(END, begin)]
    for needle in ('static void g2HoldDeliver(', 'static void g2HoldAccept(',
                   'static const char* g2HoldSim(',
                   'bool g2RegisterPageHold(', 'bool g2HoldSourceAdmitted('):
        assert code.count(needle) == 1, needle
    dev = extract_block(source, 'static void handleDevEvent(')
    assert dev.count(TAIL_START) == 1 and dev.count(TAIL_END) == 1
    tail_begin = dev.index(TAIL_START)
    tail = dev[tail_begin:dev.index(TAIL_END) + len(extract_block(dev, TAIL_END))]
    assert tail.count('g2HoldAbortFromPacket(') == 1 and 'continue' not in tail
    harness = (HERE / 'g2_hold_dispatch_harness.cpp').read_text()
    assert harness.count('// INSERT_PRODUCTION') == 1
    assert harness.count('// INSERT_SYS_EXIT_TAIL') == 1
    harness = harness.replace('// INSERT_PRODUCTION', code)
    harness = harness.replace('// INSERT_SYS_EXIT_TAIL', tail)
    with tempfile.TemporaryDirectory(prefix='hw1-g2-hold-dispatch-') as tmp:
        unit = Path(tmp) / 'test.cpp'
        exe = Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', f'-I{c}', str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)
    print('G2 hold dispatch: routes, consumers, events, aborts and sim passed')


if __name__ == '__main__':
    main()
