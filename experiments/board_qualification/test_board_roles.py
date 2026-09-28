#!/usr/bin/env python3
"""Explicit USB board-only BLE role/reconnect and optional active-clock checks.

Import, --help and --preflight never open a port. --physical requires provisioned
boards using the same private test account. No setup, pairing or radio-secret
changes. The Mac Bluetooth adapter is never used. Only the coordinator may run
this against hardware; do not use another UI/console concurrently.
"""
from __future__ import annotations

import argparse
import asyncio
import base64
import contextlib
import datetime as dt
import json
import os
from pathlib import Path
import re
import secrets
import sys
import time

HERE = Path(__file__).resolve().parent
for directory in ('p4_connectivity', 'p4_mesh', 'p4_ble_roles'):
    sys.path.insert(0, str(HERE.parent / directory))
from board_control import RolesConsole, _G2_STATUS, probe_reply
from console import ANSI
from test_mesh import MeshRunner, parse_args as mesh_args, require, normalize_mac
from test_ble import preflight, run_probe

CLOCKS = {'p4': [100, 200, 400], 's3': [80, 160, 240]}
BLE_CONFIG = ('deviceName', 'txPower', 'autoStart', 'requireAuth', 'secureChannelRequired')
PEER_CONFIG = ('name', 'autoReconnect', 'mac1', 'mac2', 'pairedBy')
POWER_CONFIG = ('mode', 'activeMhz', 'idleMhz', 'powerSaveMinutes')
QUIESCE_SECONDS = 12.0


def peer_config(peers):
    return sorted(({key: peer.get(key) for key in PEER_CONFIG} for peer in peers),
                  key=lambda peer: peer['name'])


def verify_restored_peers(saved, current):
    """Preserve every saved entry; permit only newly registered empty defaults."""
    original = {peer['name']: peer for peer in saved}
    actual = {peer['name']: peer for peer in current}
    require(len(actual) == len(current), 'Duplicate peer names in restoration reply')
    require(set(original).issubset(actual), 'A saved peer disappeared during restoration')
    for name, peer in original.items():
        require({key: actual[name].get(key) for key in PEER_CONFIG} == peer,
                'Saved peer target, owner or reconnect preference differs')
    added = []
    for name in actual.keys() - original.keys():
        peer = actual[name]
        require(peer.get('connected') is False and peer.get('autoReconnect') is False
                and not peer.get('pairedBy') and not peer.get('mac1') and not peer.get('mac2'),
                'Newly registered peer is not an unowned, disconnected empty default')
        added.append(name)
    return sorted(added)


class BoardRoles:
    def __init__(self, runner):
        self.runner = runner
        self.saved = {}
        self.touched = set()

    def command(self, key, command):
        return self.runner.command(key, command, timeout=45)

    def obj(self, key, command, field):
        return self.runner.json_command(key, command, field, timeout=45)

    def g2(self, key):
        states = _G2_STATUS.findall(ANSI.sub('', self.command(key, 'g2status')))
        require(len(states) == 1, f'{key}: missing unambiguous G2 state')
        return states[0]

    def mode(self, key):
        values = re.findall(r'BLE mode: (server|client)\b', self.command(key, 'blemode'))
        require(len(values) == 1, f'{key}: missing unambiguous BLE mode')
        return values[0]

    def snapshot(self, key):
        # Refuse a pre-existing diagnostic connection before RolesConsole's
        # lifecycle guard could retire it while reading the saved role.
        probe = probe_reply(self.command(key, 'bleprobe status'), 'status')
        require(probe.get('connected') is False and probe.get('subscribed') is False,
                f'{key}: diagnostic bridge is already in use')
        info = self.obj(key, 'bleinfo json', 'deviceName')
        peers = self.obj(key, 'blepeers json', 'peers')['peers']
        g2 = self.g2(key)
        ring = self.obj(key, 'ringstatus json', 'connectPending')
        require(info.get('connections') == 0 and g2[1:] == ('down', 'down')
                and ring.get('connected') is False and not any(p.get('connected') for p in peers),
                f'{key}: live accessory/server connection exists; refusing disruption')
        for peer in peers:
            require(isinstance(peer.get('name'), str) and re.fullmatch(r'[A-Za-z0-9_-]+', peer['name']),
                    f'{key}: unsupported peer name')
            require(not peer.get('autoReconnect') or
                    (peer.get('connectable') and bool(peer.get('pairedBy'))),
                    f'{key}: restoring auto-reconnect could change an ownerless peer')
        power = self.obj(key, 'power json', 'cpuMhz')
        require(power.get('supportedCpuMhz') == CLOCKS[key]
                and power.get('cpuMhz') in CLOCKS[key], f'{key}: unexpected clock capability/profile')
        loglink = re.findall(r'loglink: (ON|OFF)\b', self.command(key, 'loglink'))
        require(len(loglink) == 1, f'{key}: missing loglink state')
        saved = {'mode': self.mode(key), 'ble': info, 'peers': peer_config(peers),
                 'g2': list(g2), 'ringConnectPending': ring.get('connectPending'),
                 'power': power, 'loglink': loglink[0].lower()}
        require(not (info['initialized'] and g2[0] != 'off'), f'{key}: both production roles initialized')
        self.saved[key] = saved
        return saved

    def quiesce(self, key):
        self.touched.add(key)  # Mark before first mutation, even if it times out.
        self.command(key, 'loglink on')
        for peer in self.saved[key]['peers']:
            if peer['autoReconnect']:
                self.command(key, f"bleautoreconnect {peer['name']} off")
        require(not any(p.get('autoReconnect') for p in self.obj(key, 'blepeers json', 'peers')['peers']),
                f'{key}: automatic reconnect is still enabled')
        self.stop_client(key)
        return {'accessoriesDisconnected': True, 'automaticReconnectDisabled': True}

    def stop_client(self, key):
        # The public deinit error collapses many internal deferrals. Cancel
        # normal user work first, allow its shared worker to settle, then ask
        # for exactly one positive deinit. closeg2's text alone is not a fence.
        self.command(key, 'ringdisconnect')
        self.command(key, 'closeg2')
        deadline = time.monotonic() + QUIESCE_SECONDS
        while True:
            g2 = self.g2(key)
            ring = self.obj(key, 'ringstatus json', 'connectPending')
            if (g2[0] in ('off', 'idle') and g2[1:] == ('down', 'down')
                    and ring.get('connected') is False and ring.get('connectPending') is False):
                break
            require(time.monotonic() < deadline,
                    f'{key}: G2/R1 cancellation did not become idle within the bounded wait')
            time.sleep(0.2)
        self.command(key, 'g2deinit')
        self.require_idle(key)

    def require_idle(self, key):
        require(self.g2(key) == ('off', 'down', 'down'), f'{key}: G2 owner did not stop')
        ring = self.obj(key, 'ringstatus json', 'connectPending')
        require(ring.get('connected') is False and ring.get('connectPending') is False,
                f'{key}: R1 owner did not stop')

    def cycle(self, key):
        self.command(key, 'blemode client')
        self.command(key, 'g2init')
        require(self.mode(key) == 'client' and self.g2(key) == ('idle', 'down', 'down'),
                f'{key}: empty Client mode did not initialize')
        require(self.obj(key, 'bleinfo json', 'deviceName')['initialized'] is False,
                f'{key}: Server remained initialized in Client mode')
        self.stop_client(key)
        self.command(key, 'blemode server')
        self.command(key, 'openble')
        self.require_idle(key)
        info = self.obj(key, 'bleinfo json', 'deviceName')
        require(self.mode(key) == 'server' and info['initialized'] is True and info['connections'] == 0,
                f'{key}: empty Server mode did not initialize')
        return {'clientInitializedThenStopped': True, 'serverInitialized': True}

    def clocks(self, key):
        results = []
        for mhz in CLOCKS[key]:
            self.command(key, f'cpufreq {mhz}')
            actual = self.obj(key, 'power json', 'cpuMhz')
            require(actual['cpuMhz'] == mhz, f'{key}: active frequency readback differs')
            require(all(actual.get(k) == self.saved[key]['power'].get(k) for k in POWER_CONFIG),
                    f'{key}: clock command altered a power preset')
            results.append({'requestedMhz': mhz, 'actualMhz': actual['cpuMhz']})
        self.command(key, f"cpufreq {self.saved[key]['power']['cpuMhz']}")
        return {'activeReadbacks': results, 'loadTested': False, 'electricalPowerMeasured': False}

    def restore(self, key):
        if key not in self.touched:
            return {'mutationsStarted': False}
        original = self.saved[key]
        errors = []
        def attempt(command):
            try:
                self.command(key, command)
            except Exception as error:
                errors.append(f'{command.split()[0]}: {type(error).__name__}')
        # Keep flags disabled while owners are stopped, then restore the saved
        # preference explicitly because closeble selects a build default mode.
        attempt('bleprobe disconnect')
        try:
            self.stop_client(key)
        except Exception as error:
            errors.append(f'Client stop: {error}')
        attempt('closeble')
        attempt('blemode ' + original['mode'])
        if original['ble']['initialized']:
            attempt('openble')
        elif original['g2'][0] != 'off':
            attempt('g2init')
        attempt(f"cpufreq {original['power']['cpuMhz']}")
        for peer in original['peers']:
            if peer['autoReconnect']:
                attempt(f"bleautoreconnect {peer['name']} on")
        try:
            current = self.obj(key, 'bleinfo json', 'deviceName')
            require(all(current.get(k) == original['ble'].get(k) for k in BLE_CONFIG),
                    f'{key}: BLE configuration changed')
            require(current['initialized'] == original['ble']['initialized'] and self.mode(key) == original['mode'],
                    f'{key}: runtime Server state or saved role differs')
            require((self.g2(key)[0] != 'off') == (original['g2'][0] != 'off'),
                    f'{key}: runtime Client initialization differs')
            added_defaults = verify_restored_peers(original['peers'],
                self.obj(key, 'blepeers json', 'peers')['peers'])
            power = self.obj(key, 'power json', 'cpuMhz')
            require(power['cpuMhz'] == original['power']['cpuMhz']
                    and all(power.get(k) == original['power'].get(k) for k in POWER_CONFIG),
                    f'{key}: active clock or power preset differs')
        except Exception as error:
            errors.append(str(error))
        attempt('loglink ' + original['loglink'])
        require(not errors, f'{key}: restoration incomplete: ' + '; '.join(errors))
        return {'savedConfigurationVerified': True, 'runtimeInitializationVerified': True,
                'activeClockRestored': True, 'accessoryReconnectBehaviorTested': False,
                'newDefaultPeerRegistrations': added_defaults, 'fullRamStateRestored': False}


def execute(runner, credentials, ble_mac=None, clocks=False):
    test = BoardRoles(runner)
    primary_error = None
    cleanup_errors = []
    try:
        for key in runner.boards:
            runner.step(key + '_snapshot', lambda key=key: test.snapshot(key))
        target = test.saved['p4']['ble']
        require(target.get('requireAuth') is True and target.get('secureChannelRequired') is True,
                'P4 must already require authenticated encrypted BLE; no implicit provisioning')
        identity = test.obj('p4', 'espnowstatus json', 'mac')
        require(identity.get('initialized') is True, 'Existing P4 ESP-NOW must already be initialized')
        target_mac = normalize_mac(identity['mac'])
        for key in runner.boards:
            runner.step(key + '_quiesce', lambda key=key: test.quiesce(key))
        for phase in ('initial', 'after_role_cycle'):
            for key in runner.boards:
                runner.step(key + '_' + phase + '_roles', lambda key=key: test.cycle(key))
            runner.step('encrypted_' + phase, lambda: asyncio.run(run_probe(
                runner.boards['s3'], credentials, expected_name=target['deviceName'],
                expected_mac=target_mac, expected_ble_mac=ble_mac,
                run_root=runner.run_dir / 'encrypted', timeout=65, scan_seconds=5)))
        if clocks:
            for key in runner.boards:
                runner.step(key + '_active_clocks', lambda key=key: test.clocks(key))
    except BaseException as error:
        primary_error = error
    finally:
        for key in reversed(tuple(runner.boards)):
            try:
                runner.step(key + '_restore', lambda key=key: test.restore(key))
            except BaseException as error:
                cleanup_errors.append(f'{key}: {error}')
        runner.results['restorationErrors'] = cleanup_errors
        runner.results['status'] = 'passed' if primary_error is None and not cleanup_errors else 'failed'
        if primary_error is not None:
            runner.results['error'] = str(primary_error)
        runner.save()
    if primary_error is not None:
        raise primary_error
    require(not cleanup_errors, 'One or more board restorations failed; inspect private results')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--preflight', action='store_true')
    mode.add_argument('--physical', action='store_true')
    parser.add_argument('--p4-port')
    parser.add_argument('--s3-port')
    parser.add_argument('--credentials', type=Path)
    parser.add_argument('--ble-mac', help='Optional explicitly observed P4 BLE advertisement address; not its ESP-NOW MAC')
    parser.add_argument('--clocks', action='store_true', help='Also exercise transient active clock readbacks; no sleep/preset changes')
    parser.add_argument('--run-root', type=Path, default=HERE / 'private' / 'board-roles')
    args = parser.parse_args(argv)
    if args.preflight:
        print(json.dumps(preflight(), indent=2))
        return 0
    if not (args.p4_port and args.s3_port and args.credentials):
        parser.error('--physical requires --p4-port, --s3-port and --credentials')
    require(args.p4_port != args.s3_port, 'Select two distinct serial ports')
    if args.ble_mac:
        args.ble_mac = normalize_mac(args.ble_mac)
    require(not args.credentials.is_symlink() and args.credentials.is_file()
            and args.credentials.stat().st_mode & 0o077 == 0, 'Credentials must be a private regular file (mode 0600)')
    loaded = json.loads(args.credentials.read_text())
    credentials = {key: loaded.get(key) for key in ('username', 'password', 'ble_secret')}
    require(all(isinstance(value, str) and value for value in credentials.values()), 'Missing private test credentials')
    private_values = [value for value in loaded.values() if isinstance(value, str) and value]
    private_values += [base64.b64encode(value.encode()).decode() for value in private_values]
    run_id = secrets.token_hex(4)
    run = args.run_root / (dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ') + '-' + run_id)
    run.mkdir(parents=True, mode=0o700)
    os.chmod(run, 0o700)
    config = {'p4': {'port': args.p4_port}, 's3': {'port': args.s3_port}}
    with contextlib.ExitStack() as stack:
        boards = {key: stack.enter_context(RolesConsole(spec['port'], run / (key + '.log'),
                  secrets=private_values, completion='hardwareone')) for key, spec in config.items()}
        runner = MeshRunner(boards, config, loaded, run, run_id, mesh_args([]))
        runner.results['scope'] = 'board-only role lifecycle and encrypted BLE initial/reconnect; optional active clock readback'
        runner.results['options'] = {'clocks': args.clocks, 'macBluetoothUsed': False, 'provisioned': False}
        try:
            for key in boards:
                runner.step(key + '_login', lambda key=key: runner.login(key))
            execute(runner, credentials, args.ble_mac, args.clocks)
        except BaseException as error:
            runner.results.update(status='failed', error=str(error))
            runner.save()
            print(runner.safe(f'FAIL {type(error).__name__}: {error}'), file=sys.stderr)
            print('Private results: ' + str(run / 'results.json'))
            return 1
    print('PASS; private results: ' + str(run / 'results.json'))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
