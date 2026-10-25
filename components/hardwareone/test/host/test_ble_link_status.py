#!/usr/bin/env python3
"""Run the Bluetooth link status words (BLE_LinkStatusCore.h) as shipped.

Every role, pipe and console input combination maps to the documented words
(docs/PHONE_CONSOLE_VS_PIPE.md), and every status label BLE_PhonePipe.cpp can
set is classified, so a new failure label can never read as success. The
phone link block (v2): the console stream comes first in the console word, its
own words for the block, relay on/off/unknown, a name for every pipe error code
0..18 (16 service-off, 17 unsupported, 18 paused; unknown codes are "other"),
and the bounded text lines. Source checks pin the step order, the lock-free
accessors, the advertising re-arm, the console view counting the stream's row
and the snapshot's mirrors of the pipe's own enums.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def pipe_labels(pipe: str) -> list[str]:
    """Every static text the pipe stores as its status."""
    labels = set()
    for pattern in (r'setStep\(Step::\w+,\s*"([^"]+)"', r'fail\((?:now|millis\(\)),\s*[^,]+,\s*"([^"]+)"',
                    r'cleanupExpired\(now,\s*"([^"]+)"', r'\bstatus\s*=\s*"([^"]+)"'):
        labels.update(re.findall(pattern, pipe))
    # Ternaries inside setStep: setStep(Step::Idle,a?"x":b?"y":...)
    for call in re.findall(r'setStep\(Step::\w+,([^;]*?),\s*(?:now|jobAt|millis\(\))\)', pipe):
        labels.update(re.findall(r'"([^"]+)"', call))
    # The pipe start's own refusal and the initial value.
    labels.update(re.findall(r'std::atomic<const char\*> status\{"([^"]+)"\}', pipe))
    return sorted(labels)


def generated(component: Path) -> str:
    pipe = (component / 'BLE_PhonePipe.cpp').read_text()
    labels = pipe_labels(pipe)
    require(len(labels) >= 25, f'found only {len(labels)} pipe labels; the extractor broke')
    body = ', '.join('"' + l.replace('\\', '\\\\').replace('"', '\\"') + '"' for l in labels)
    harness = (HERE / 'ble_link_status_harness.cpp').read_text()
    return harness.replace('// INSERT_LABELS', f'static const char* const kPipeLabels[] = {{{body}}};')


def check_sources(component: Path) -> None:
    pipe = (component / 'BLE_PhonePipe.cpp').read_text()
    require('std::atomic<Step> step{Step::Idle};' in pipe and 'std::atomic<bool> enrolling{false};' in pipe,
            'the step and enrolling flag are atomic for lock-free status reads')
    require('uint8_t phonePipeStep(){return uint8_t(step.load());}' in pipe and
            'bool phonePipeEnrolling(){return enrolling.load() || pairState.load()==PhonePairWindow::Open;}' in pipe
            and 'std::atomic<PhonePairWindow> pairState{PhonePairWindow::None};' in pipe,
            'lock-free accessors (`phone pair new`\'s window reads as enrolling)')
    require("keep Step in BLE_LinkStatusCore.h's order" in pipe, 'Step and PipeStep are pinned together')
    bt = (component / 'Bluetooth.cpp').read_text()
    helper = extract_block(bt, 'static bool bleAdvertiseIfRoomLocked() ')
    require('activeConnectionCount >= BLE_MAX_CONNECTIONS' in helper and 'sBleAdvertising.store(true)' in helper,
            'advertising re-arms only below the maximum')
    require('broadcastOutput' not in helper and 'BLE_DEBUGF' not in helper, 'the BTC-side helper never logs')
    connect = bt[bt.index('void onConnect('):bt.index('void onDisconnect(')]
    require('sBleAdvertising.store(false);' in connect and 'bleAdvertiseIfRoomLocked()' in connect,
            'a connect re-arms advertising while another app fits')
    disconnect = bt[bt.index('void onDisconnect('):]
    disconnect = disconnect[:disconnect.index('BLE_DEBUGF')]
    require('bleAdvertiseIfRoomLocked()' in disconnect and 'startBLEAdvertising()' not in disconnect,
            'a disconnect re-arms without the logging start path')
    start = extract_block(bt, 'bool startBLEAdvertising() ')
    require('BLE_STATE_CONNECTED' not in start, 'a connected app no longer blocks advertising')
    view = extract_block(bt, 'BleConsoleView bleConsoleView() ')
    require('BleLifecycleGuard lifecycle(0);' in view and 'bleIsPeerLinkType' in view,
            'the console view never blocks and counts only app clients')
    require('if (!blePipeRow(c) && !server && !view.attached) continue;' in view,
            "the console view counts the phone link's console row in every client role")
    link = (component / 'BLE_LinkStatus.cpp').read_text()
    require('"hw1link::StreamState mirrors PipeConsoleState"' in link and
            '"one end-reason name per pipe error code"' in link,
            'the snapshot pins its stream-state and end-reason mirrors at compile time')
    inputs = extract_block(link, 'hw1link::PhoneLinkInputs bleLinkPhoneInputs()')
    require('phonePipeSecret.length()' in inputs and 'phonePipeSecret.c_str()' not in inputs,
            "the secret's kind is read from its length and mark, never its bytes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone (mutation runs)')
    args = parser.parse_args()
    component = (args.component or COMPONENT).resolve()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-link-status-') as directory:
        source = Path(directory) / 'link.cpp'
        binary = Path(directory) / 'link'
        source.write_text(generated(component))
        subprocess.run([args.cxx, *flags, '-I', str(component), str(source), '-o', str(binary)],
                       check=True, timeout=180)
        print(subprocess.check_output([str(binary)], text=True, timeout=60).strip())
    check_sources(component)
    print('BLE link status: role, pipe and console words (console stream first); phone link '
          'block words, end reasons 0..18 and lines; every pipe label classified; lock-free '
          'accessors; advertising re-arm')


if __name__ == '__main__':
    main()
