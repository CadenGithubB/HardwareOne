#!/usr/bin/env python3
"""Switch to server mode (`openble`) and back to the glasses (`openg2 saved`).

A role switch away from the client host is not a user disconnect of its peers.
The unit runs the production reconnect scheduler and tick (BLE_Peers.cpp),
`openconsole` (Bluetooth.cpp cmd_openconsole), the ring's disconnect, logical down,
admission and `ring disconnect` (G2_Ring.cpp), the glasses' disconnect intent,
admission, `openg2` and g2ConnectSaved (G2_Glasses.cpp) and the phone pipe's
peer ops (BLE_PhonePipe.cpp). The role owners (initBluetooth, deinitG2Client,
initG2Client) and the pipe's worker are fakes in the production order, which
the text checks below and test_g2_client_lifecycle.py pin.

Cases: the ring and the phone pipe come back after openble then openg2 saved;
a ring the user disconnected, before openble or during server mode, stays down;
with auto-reconnect off, a ring or phone pipe that was up comes back once and a
ring that was down does not; a boot in server mode followed by openg2 saved
arms the ring and the phone; a refused openble lets the glasses and ring come
back on the running client host. Ten minutes in server mode start nothing,
no client host included.

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


def between(text: str, start: str, end: str) -> str:
    """From `start` (inclusive) to the next `end` (exclusive); both must exist."""
    require(text.count(start) == 1, f'one {start!r}')
    i = text.index(start)
    return text[i:text.index(end, i)]


def line(text: str, prefix: str) -> str:
    hits = [l for l in text.splitlines() if l.startswith(prefix)]
    require(len(hits) == 1, f'one line starting {prefix!r}')
    return hits[0]


def blocks(text: str, *markers: str) -> list:
    return [extract_block(text, marker) for marker in markers]


def unit(component: Path) -> str:
    peers_h = (component / 'BLE_Peers.h').read_text()
    peers = (component / 'BLE_Peers.cpp').read_text()
    bluetooth_h = (component / 'Bluetooth.h').read_text()
    bluetooth = (component / 'Bluetooth.cpp').read_text()
    glasses_h = (component / 'G2_Glasses.h').read_text()
    glasses = (component / 'G2_Glasses.cpp').read_text()
    ring = (component / 'G2_Ring.cpp').read_text()
    pipe = (component / 'BLE_PhonePipe.cpp').read_text()

    types = '\n'.join([
        *[l for l in bluetooth_h.splitlines() if l.startswith('#define BLE_MODE_')],
        extract_block(bluetooth_h, 'enum class BleRoleTransition : uint8_t') + ';',
        between(peers_h, 'enum BlePeerKind : uint8_t {', '// Compile-time descriptor for a peer.'),
        extract_block(peers_h, 'struct BlePeerSpec') + ';',
        extract_block(peers_h, 'struct BlePeerReconnectSnapshot') + ';',
        # The registry's declarations, as callers see them.
        between(peers_h, 'bool bleRegisterPeer(const BlePeerSpec& spec);', '// MAC auto-save'),
        extract_block(glasses_h, 'enum G2Eye') + ';',
        extract_block(glasses_h, 'enum class BleConnectKind') + ';',
        extract_block(glasses_h, 'struct BleConnectJob') + ';',
    ])
    scheduler = '\n\n'.join([
        extract_block(peers, 'struct PeerReconnectRuntime') + ';',
        'PeerReconnectRuntime sReconnect[BLE_PEER_MAX];',
        'portMUX_TYPE sReconnectMux = portMUX_INITIALIZER_UNLOCKED;',
        *blocks(peers, 'uint32_t nextReconnectGeneration(', 'void reconnectBumpIntentLocked(',
                'bool savedTargetMatches(', 'bool connectRequestIsCurrentLocked(',
                'void noteLinkUpLocked(', 'bool blePeerReconnectSnapshot(',
                'bool blePeerConnectRequestIsCurrent(', 'bool blePeerBeginManualConnectRequest('),
        between(peers, 'static const BlePeerSpec* gPeerByKind', '// MAC auto-save'),
        between(peers, 'static constexpr uint32_t kAdmissionBusyRetryMs', 'struct ReconnectAdmissionClaim'),
        extract_block(peers, 'struct ReconnectAdmissionClaim') + ';',
        *blocks(peers, 'static uint32_t bleReconnectBackoffMs(', 'static bool peerHasConnectAdmission(',
                'static BlePeerConnectAdmission sanitizeAdmission(', 'static const char* admissionName(',
                'static BlePeerConnectAdmission requestPeerConnectAdmission(',
                'static void fillAdmissionClaimLocked(', 'static bool claimDueAdmission(',
                'static bool commitAdmission(', 'void blePeerNoteUserDisconnect(',
                'void blePeerNoteLinkLost(', 'void blePeerNoteLinkUp(',
                'void blePeerRequestReseek(BlePeerKind kind) ', 'void bleAutoReconnectTick(void)',
                'void bleArmAutoReconnectPeers(void)'),
    ])
    rings = '\n\n'.join([
        between(ring, 'static bool ringPeerConnectSavedThunk()', 'struct G2RingState {'),
        between(ring, 'struct G2RingState {', '// Fixed, task-level transition fence'),
        line(ring, 'static bool gRingUpEventPublished'),
        extract_block(ring, 'static bool ringPublishedConnected() '),
        between(ring, 'static volatile bool  gRingConnectTaskActive', '// Failed-connect visibility.'),
        extract_block(ring, 'static bool ringConnectGateOpen(uint32_t* generationOut) '),
        extract_block(ring, 'struct RingDownTransition ') + ';',
        *blocks(ring, 'static RingDownTransition ringBeginLogicalDown(',
                'void g2RingDisconnect(bool userInitiated) ',
                'static BlePeerConnectAdmission ringPeerConnectSavedAdmissionThunk(\n'
                '    const BlePeerConnectRequest& request) ',
                'bool g2RingIsConnected() ', 'static const char* cmd_closering('),
    ])
    glasses_state = '\n\n'.join([
        extract_block(glasses, 'struct G2LinkSnapshot ') + ';',
        line(glasses, 'static uint8_t gG2PublishedTempleMask'),
        # The topology snapshot, from the published temples only.
        'static G2LinkSnapshot g2LinkSnapshot() {\n'
        '  G2LinkSnapshot out{};\n'
        '  out.left = out.leftReady = (gG2PublishedTempleMask & 0x01) != 0;\n'
        '  out.right = out.rightReady = (gG2PublishedTempleMask & 0x02) != 0;\n'
        '  return out;\n}',
    ])
    g2 = '\n\n'.join([
        glasses_state,
        between(glasses, 'static bool g2PeerConnectSavedThunk()', '// Registers the built-in G2 pages'),
        *blocks(glasses, 'static bool g2PublishedPairConnected() ', 'static bool g2RoleAdmissionOpen() ',
                'static void copyBleAddressText(char (&dst)[BLE_PEER_ADDRESS_TEXT_CAPACITY],',
                'static void blePeerRequestToJob(const BlePeerConnectRequest& request,',
                'static void g2PublishDisconnectIntent(bool userInitiated = false) ',
                'bool isG2Connected() ', 'bool g2ConnectSaved() ',
                'static BlePeerConnectAdmission g2PeerConnectSavedAdmissionThunk(\n'
                '    const BlePeerConnectRequest& request) ',
                'static const char* cmd_g2connect(const String& argsInput) '),
    ])
    pipe_spec = '\n'.join(re.search(r'^const BlePeer%s\{[^;]*\};$' % k, pipe, re.M).group(0)
                          for k in ('Ops ops', 'Spec spec'))
    bt = '\n\n'.join(blocks(bluetooth, 'bool bleSubsystemActive() ',
                            'static const char* cmd_openconsole(const String& argsInput) '))
    harness = (HERE / 'ble_role_switch_reconnect_harness.cpp').read_text()
    for marker, text in (('// INSERT_TYPES', types), ('// INSERT_PEERS', scheduler), ('// INSERT_RING', rings),
                         ('// INSERT_GLASSES', g2), ('// INSERT_PIPE_SPEC', pipe_spec),
                         ('// INSERT_BLUETOOTH', bt)):
        require(harness.count(marker) == 1, marker)
        harness = harness.replace(marker, text)
    return harness


def strip_comments(code: str) -> str:
    return '\n'.join(l.split('//', 1)[0] for l in code.splitlines())


def check_text(component: Path) -> None:
    """The role owners the unit fakes, in their production order."""
    glasses = (component / 'G2_Glasses.cpp').read_text()
    bluetooth = (component / 'Bluetooth.cpp').read_text()
    stack = (component / 'BLE_CentralStack.cpp').read_text()
    pipe = (component / 'BLE_PhonePipe.cpp').read_text()
    # bleStackUp arms the peers last, after registering every device owner's
    # peer; nothing else arms them. The glasses runtime registers its own peer
    # again on init (idempotent) but never arms.
    up = strip_comments(extract_block(stack, 'bool bleStackUp() '))
    call = 'bleArmAutoReconnectPeers();'
    require(up.count(call) == 1, 'bleStackUp arms the peers once')
    at = up.index(call)
    for before in ('registerDevicePeers();', 'bleSetupWorkerStart()'):
        require(up.index(before) < at, f'peers are armed after {before}')
    init = strip_comments(extract_block(glasses, 'bool initG2Client() '))
    require(call not in init and 'bleStackUp()' in init and 'g2RegisterBlePeer();' in init,
            'initG2Client opens the stack and registers its peer; it never arms')
    callers = sorted(p.name for p in component.glob('*.cpp')
                     if 'bleArmAutoReconnectPeers(' in strip_comments(p.read_text()))
    require(callers == ['BLE_CentralStack.cpp', 'BLE_Peers.cpp'], 'bleStackUp is the only caller')
    # deinitG2Client takes the glasses down without a user disconnect and
    # leaves the ring and the phone alone: they are device owners of their own.
    deinit = strip_comments(extract_block(glasses, 'bool deinitG2Client() '))
    require('g2Disconnect();' in deinit, 'deinitG2Client disconnects the glasses without a user stamp')
    for banned in ('g2RingDisconnect(', 'phonePipeStop(', 'bluetoothHalDeinit(', 'bleStackDown('):
        require(banned not in deinit, f'deinitG2Client never calls {banned}')
    # initBluetooth closes the glasses runtime, then takes the shared stack down.
    server = strip_comments(extract_block(bluetooth, 'bool initBluetooth() '))
    require(server.index('deinitG2Client()') < server.index('bleStackDown()'),
            'initBluetooth closes the glasses before the stack')
    # The pipe's worker notes a READY link that ended (a stop included) as lost.
    require('if(lost)blePeerNoteLinkLost(BLE_PEER_PHONE);' in pipe, 'the pipe notes a lost link')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--component', type=Path, help='a copy of components/hardwareone to read production from')
    args = parser.parse_args()
    component = (args.component or HERE.parents[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='hw1-role-switch-') as tmp:
        path = Path(tmp) / 'role_switch.cpp'
        binary = Path(tmp) / 'role_switch'
        path.write_text(unit(component))
        command = [os.environ.get('CXX', 'clang++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-Wno-unused-function', '-Wno-unused-const-variable', '-Wno-unused-variable',
                   '-Wno-unused-parameter', str(path), '-o', str(binary)]
        if args.sanitize:
            command[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                            '-fno-omit-frame-pointer', '-g']
        subprocess.run(command, check=True)
        # Behaviour first, so a mutated --component fails on what the firmware does.
        subprocess.run([str(binary)], check=True, timeout=120)
    check_text(component)
    print('BLE role switch: text checks passed')
