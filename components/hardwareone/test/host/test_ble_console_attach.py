#!/usr/bin/env python3
"""Run the app console's attach decisions (BLE_ConsoleAttachCore.h) as shipped.

In G2 client mode the console's command service rides the phone pipe's link:
only the authenticated, READY pipe's link becomes a console slot, found by the
pipe's live connection id and keyed by its generation. The harness runs every
input combination (mode, setting, secret, pipe record, slot, joined, closed
memory), the named sequences (link up before or after attach, generation change,
detach, connId reuse, secret removed, a missed disconnect, the app joining),
the app's first write joining the slot, `bledisconnect`'s end of a joined
session, and a random walk over a model of the four C6 links in which READY
may race the pipe link's drop. The text check keeps the core pure.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def check_sources(component: Path) -> None:
    core = (component / 'BLE_ConsoleAttachCore.h').read_text()
    includes = re.findall(r'^#include\s+(\S+)', core, re.MULTILINE)
    require(includes == ['<cstdint>'], f'the core stays pure (no BLE, OS or Arduino): {includes}')
    require('enum class Mode : uint8_t { Off, Server, ClientRegistered };' in core, 'the console modes')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone (mutation runs)')
    args = parser.parse_args()
    component = (args.component or COMPONENT).resolve()
    require(bool(args.cxx), 'a host C++17 compiler is required')
    flags = ['-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-g']
    check_sources(component)
    with tempfile.TemporaryDirectory(prefix='hw1-console-attach-') as directory:
        binary = Path(directory) / 'attach'
        subprocess.run([args.cxx, *flags, '-I', str(component), str(HERE / 'ble_console_attach_harness.cpp'),
                        '-o', str(binary)], check=True, timeout=180)
        print(subprocess.check_output([str(binary)], text=True, timeout=120).strip())
    print('BLE console attach: client mode admits only the ready pipe link by connection id; '
          'every combination, sequence and random walk passed')


if __name__ == '__main__':
    main()
