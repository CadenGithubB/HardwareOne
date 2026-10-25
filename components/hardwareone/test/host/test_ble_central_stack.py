#!/usr/bin/env python3
"""Run the shared BLE central stack owner (BLE_CentralStack.cpp) against fakes.

BLE_CentralStack.cpp is the one owner of the Bluedroid host and its controller
link, the setup worker, the scan object, the GAP/GATTC observers and the
console service on the client host. ble_central_stack_harness.cpp compiles its
whole `#if ENABLE_BLUETOOTH` region with the real HAL_Bluetooth.h and
BLE_CentralStack.h, the BleRoleTransition states (Bluetooth.h), the glasses'
MTU preference (G2_Glasses.h) and Arduino's constrain(). Every boundary call is
a recording fake, so each case asserts the exact ordered events: the bring-up
from an absent host (native and Hosted), the already-ready no-op, the
refusals before any HAL call, the normalization of a stale generation and each
of its failures, the memory refusal, the partial-host rollbacks, TX power,
bleStackDown()'s refusals, teardown and setup-worker release, a repeated down,
a lost host and a busy role claim. Each lane in LANES compiles the unit with
the ENABLE_* flags it branches on.

The fake phone pipe acknowledges only a host the HAL reports stopped(), as
the pipe does, so a Hosted host that never ran (absent, not stopped: the P4's
first start) must come up without the handshake.

The text checks pin what the cases imply about the source: the peers are
registered before the auto-reconnect arm; the phone pipe is asked only after
a verified teardown or by bleStackDown() on an absent host, never by
bleStackUp() itself; the normalize step withdraws the ready flag and the scan
object before its teardown; bleStackDown() refuses for the server and every
open device owner before any HAL call, closes admission before the host
teardown and releases the setup worker after it; the file creates no task and
places nothing in PSRAM.

--component points every production read at another copy of
components/hardwareone (mutation runs).
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
HARNESS = HERE / 'ble_central_stack_harness.cpp'
ARDUINO_H = HERE.parents[2] / 'arduino' / 'cores' / 'esp32' / 'Arduino.h'

# The flags BLE_CentralStack.cpp branches on; ENABLE_BLUETOOTH=1 in every lane.
LANES = {
    'full': dict(ENABLE_G2_GLASSES=1, ENABLE_R1_HEALTH=1, ENABLE_BLE_PHONE_PIPE=1, ENABLE_BLE_COEX_RECORDS=1),
    'no_pipe': dict(ENABLE_G2_GLASSES=1, ENABLE_R1_HEALTH=1, ENABLE_BLE_PHONE_PIPE=0, ENABLE_BLE_COEX_RECORDS=1),
    'glasses_no_ring': dict(ENABLE_G2_GLASSES=1, ENABLE_R1_HEALTH=0, ENABLE_BLE_PHONE_PIPE=1,
                            ENABLE_BLE_COEX_RECORDS=1),
    'phone_only': dict(ENABLE_G2_GLASSES=0, ENABLE_R1_HEALTH=0, ENABLE_BLE_PHONE_PIPE=1, ENABLE_BLE_COEX_RECORDS=0),
}
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include\b.*$', re.M)


def blank_includes(text: str) -> str:
    """Blank the #include lines in place, so line numbers stay the file's."""
    return INCLUDE.sub('', text)


def line_number(text: str, snippet: str) -> int:
    return text.count('\n', 0, text.index(snippet)) + 1


def one_line(text: str, pattern: str, what: str) -> str:
    hits = re.findall(r'^' + pattern + r'$', text, re.M)
    require(len(hits) == 1, f'one {what}')
    return hits[0]


def guarded_region(text: str, opener: str) -> str:
    """The `opener` line through its matching #endif, nested blocks included."""
    lines = text.splitlines(keepends=True)
    starts = [i for i, l in enumerate(lines) if l.rstrip() == opener]
    require(len(starts) == 1, f'one {opener!r} line')
    depth = 0
    for i in range(starts[0], len(lines)):
        directive = lines[i].lstrip()
        if re.match(r'#\s*if(n?def)?\b', directive):
            depth += 1
        elif re.match(r'#\s*endif\b', directive):
            depth -= 1
            if depth == 0:
                return ''.join(lines[starts[0]:i + 1])
    raise AssertionError(f'{opener!r} has no matching #endif')


def unit(component: Path) -> str:
    """The harness with production text spliced in under #line directives."""
    stack = (component / 'BLE_CentralStack.cpp').read_text()
    region = guarded_region(stack, '#if ENABLE_BLUETOOTH')
    bluetooth_h = (component / 'Bluetooth.h').read_text()
    role = extract_block(bluetooth_h, 'enum class BleRoleTransition : uint8_t') + ';'
    glasses_h = (component / 'G2_Glasses.h').read_text()
    mtu = one_line(glasses_h, r'static constexpr uint16_t G2_BLE_LOCAL_MTU_PREF = \d+;', 'G2 MTU preference')
    arduino = ARDUINO_H.read_text()
    constrain = one_line(arduino, r'#define constrain\(amt, low, high\) .*', 'Arduino constrain()')
    hal_h = (component / 'HAL_Bluetooth.h').read_text()
    stack_h = (component / 'BLE_CentralStack.h').read_text()
    splices = (
        ('// INSERT_CONSTRAIN', ARDUINO_H, line_number(arduino, constrain), constrain),
        ('// INSERT_HAL_BLUETOOTH', component / 'HAL_Bluetooth.h', 1, blank_includes(hal_h)),
        ('// INSERT_ROLE_TRANSITION', component / 'Bluetooth.h', line_number(bluetooth_h, role[:-1]), role),
        ('// INSERT_G2_MTU', component / 'G2_Glasses.h', line_number(glasses_h, mtu), mtu),
        ('// INSERT_STACK_HEADER', component / 'BLE_CentralStack.h', 1, blank_includes(stack_h)),
        ('// INSERT_PRODUCTION', component / 'BLE_CentralStack.cpp', line_number(stack, region),
         blank_includes(region)),
    )
    original = HARNESS.read_text()
    harness = original
    for marker, source, first, text in splices:
        require(original.count(marker) == 1, f'one {marker} in the harness')
        # Diagnostics name the production file and line; the harness resumes
        # its own numbering on the line after the marker.
        resume = line_number(original, marker) + 1
        harness = harness.replace(
            marker, f'#line {first} "{source}"\n{text.rstrip()}\n#line {resume} "{HARNESS}"')
    return harness


def strip_comments(code: str) -> str:
    return '\n'.join(l.split('//', 1)[0] for l in code.splitlines())


def check_text(component: Path) -> None:
    stack = (component / 'BLE_CentralStack.cpp').read_text()
    # The stack runs on its callers' tasks (the setup worker is
    # BLE_SetupWorker's) and places nothing in PSRAM.
    for banned in ('xTaskCreate', 'EXT_RAM'):
        require(banned not in stack, f'BLE_CentralStack.cpp contains {banned}')
    up = strip_comments(extract_block(stack, 'bool bleStackUp() '))
    down = strip_comments(extract_block(stack, 'bool bleStackDown() '))
    teardown_fn = strip_comments(extract_block(stack, 'bool hostTeardown() '))
    rollback_fn = strip_comments(extract_block(stack, 'void rollbackPartialHost('))
    # Every device owner's peer spec is registered before the auto-reconnect arm.
    for call in ('registerDevicePeers();', 'bleArmAutoReconnectPeers();'):
        require(up.count(call) == 1, f'bleStackUp calls {call} once')
    require(up.index('registerDevicePeers();') < up.index('bleArmAutoReconnectPeers();'),
            'bleStackUp registers the peers before it arms them')
    # The phone pipe acknowledges only a host the HAL reports stopped(), so it
    # is asked after a verified teardown (hostTeardown, a successful rollback)
    # and, result ignored, by bleStackDown on an absent host; never by
    # bleStackUp itself, where a Hosted host that never ran is absent but not
    # stopped.
    handshake = 'phonePipeHostStopped('
    require(strip_comments(stack).count(handshake) == 3, f'three {handshake} calls in BLE_CentralStack.cpp')
    require(handshake not in up, 'bleStackUp never asks the phone pipe itself')
    require(teardown_fn.count(handshake) == 1 and
            teardown_fn.index('bluetoothHalStatus().stopped()') < teardown_fn.index(handshake),
            'hostTeardown asks the pipe once, after the stopped() check')
    require(re.search(r'if \(rollback\.success\) \{[^}]*\(void\)phonePipeHostStopped\(\);', rollback_fn),
            'rollbackPartialHost asks the pipe only after a successful rollback, ignoring its answer')
    require(re.search(r'\} else \{\s*\(void\)phonePipeHostStopped\(\);\s*\}', down),
            'bleStackDown asks the pipe in its absent-host branch only, ignoring its answer')
    # The normalize step withdraws admission and the scan object before its
    # teardown (the scan pointer is not observable through the API).
    normalize = up.index('hostTeardown()')
    for withdrawn in ('sReady.store(false', 'sScan = nullptr;'):
        require(withdrawn in up and up.index(withdrawn) < normalize,
                f'bleStackUp sets {withdrawn} before the normalize teardown')
    # bleStackDown refuses for the server and each open device owner before it
    # touches the ready flag, the HAL or the phone pipe's host handshake.
    effects = [down.index(s) for s in ('sReady.store(', 'bluetoothHal', 'hostTeardown()',
                                       'phonePipeHostStopped()') if s in down]
    require(effects, 'bleStackDown has effects')
    for owner, close in (('isBleServerInitialized()', 'closeconsole'), ('isG2ClientInitialized()', 'closeg2'),
                         ('g2RingIsOpen()', 'closering'), ('phonePipeActive()', 'closephone')):
        refusal = re.search(r'if \(' + re.escape(owner) + r'\)\s*\{?\s*return refuse\("[^"]*\'' + close + "'",
                            down)
        require(refusal is not None, f'bleStackDown refuses while {owner}, naming {close!r}')
        require(refusal.start() < min(effects), f'bleStackDown checks {owner} before any HAL call')
    # Admission closes before the host goes; the phone worker (parked across
    # `closephone` until the host is down) and then the setup worker go after it.
    teardown = down.index('hostTeardown()')
    require(down.count('sReady.store(false') == 1 and down.index('sReady.store(false') < teardown,
            'bleStackDown clears the ready flag before the host teardown')
    require(down.count('bleSetupWorkerRelease(') == 1 and teardown < down.index('bleSetupWorkerRelease('),
            'bleStackDown releases the setup worker after the host teardown')
    phone_worker = 'phonePipeWorkerRelease('
    require(strip_comments(stack).count(phone_worker) == 1 and phone_worker in down,
            f'one {phone_worker} call, in bleStackDown')
    released = down.index(phone_worker)
    require(down.index('(void)phonePipeHostStopped();') < released < down.index('bleSetupWorkerRelease('),
            'bleStackDown releases the phone worker after the host is down, before the setup worker')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone to read production from')
    args = parser.parse_args()
    component = (args.component or HERE.parents[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='hw1-ble-central-stack-') as tmp:
        source = Path(tmp) / 'ble_central_stack.cpp'
        source.write_text(unit(component))
        # Behaviour first, so a mutated --component fails on what the firmware does.
        for lane, flags in LANES.items():
            binary = Path(tmp) / f'ble_central_stack_{lane}'
            command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-function', '-Wno-unused-parameter', '-DENABLE_BLUETOOTH=1',
                       *(f'-D{name}={value}' for name, value in flags.items()), str(source), '-o', str(binary)]
            if args.sanitize:
                command[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                                '-fno-omit-frame-pointer', '-g']
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True, timeout=120)
    check_text(component)
    print('BLE central stack: text checks passed')
