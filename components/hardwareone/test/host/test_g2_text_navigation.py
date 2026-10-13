#!/usr/bin/env python3
"""Run production decoded TextEvent/SysEvent routing and callback ownership.

The actual publication/snapshot functions and handleDevEvent gesture branches
are compiled unchanged. Protobuf decoding, RTOS queueing and BLE are outside
this test; the queue shim delivers the callback selected by production routing.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', type=Path, default=HERE.parents[1])
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    component = args.component.resolve()
    sys.path.insert(0, str(component / 'test/host'))
    from test_web_batch_handlers import extract_block
    source = (component / 'G2_Glasses.cpp').read_text()
    functions = '\n\n'.join(extract_block(source[source.rindex(marker):], marker) for marker in (
        'static inline uint32_t g2PresentationEpochCurrent(',
        'static inline bool pageSwapInFlight(',
        'static uint32_t g2TextViewNextSerialLocked(',
        'static void g2TextViewDisarmLocked(',
        'static bool g2TextViewPublishForLifecycle(',
        'static bool g2TextViewSnapshotForPacket('))
    functions = extract_block(source, 'struct G2TextViewClaim') + ';\n' + functions
    dev = extract_block(source, 'static void handleDevEvent(')
    text = extract_block(dev, '} else if (field == 2)')
    text = text[text.index('// Hold copies go to'):text.rfind('}')]
    sys_event = extract_block(dev, '} else if (field == 3)')
    sys_event = sys_event[sys_event.index('G2TextViewClaim textClaim{};'):sys_event.index('bool ownShutdownEcho = false;')]
    # Connect the shared callback-slot test to both shipping page producers.
    # Both successful CREATE branches arm the same publication in cleanup.
    worker_start = source.index('  } else if (args->kind == PSK_MULTITEXT) {', source.index('enum PageSwapKind'))
    # The first occurrence may be argument cleanup; select the actual CREATE branch.
    multi_start = source.index('    // CreateStartUpPage with N TextObject children', worker_start)
    multi = source[multi_start:source.index('  } else /* PSK_LIST_TEXT */ {', multi_start)]
    assert 'armRequestedTextCallbacks = true;' in multi
    cleanup = source[source.index('cleanup:', multi_start):]
    assert 'args->exitFn, args->tapFn, lifecycleEpoch,\n          committedPresentationEpoch, /*isTextPage*/ true' in cleanup
    harness = (HERE / 'g2_text_navigation_harness.cpp').read_text()
    for marker, code in (('// INSERT_PRODUCTION', functions), ('// INSERT_TEXT_ROUTE', text), ('// INSERT_SYS_ROUTE', sys_event)):
        assert harness.count(marker) == 1
        harness = harness.replace(marker, code)
    with tempfile.TemporaryDirectory(prefix='hw1-g2-text-navigation-') as tmp:
        unit, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
        unit.write_text(harness)
        cmd = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(unit), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=120)
        subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == '__main__':
    main()
