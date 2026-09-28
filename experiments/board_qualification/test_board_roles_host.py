"""Offline state-machine checks; no ports, credentials or radio access."""
import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_board_roles as roles


class FakeBoard:
    pass


class FakeRunner:
    def __init__(self):
        self.boards = {'p4': FakeBoard(), 's3': FakeBoard()}
        self.run_dir = Path('/unused-private-test')
        self.results = {}
        self.commands = []
        self.restore_fail = None
        self.probe_count = 0
        self.state = {}
        for key in self.boards:
            self.state[key] = {
                'mode': 'client', 'g2': 'idle', 'loglink': 'off',
                'ble': {'schema': 1, 'deviceName': 'test', 'initialized': False,
                        'connections': 0, 'txPower': 7, 'autoStart': True,
                        'requireAuth': True, 'secureChannelRequired': True},
                'peers': [{'name': 'g2', 'connectable': True, 'connected': False,
                           'autoReconnect': True, 'pairedBy': 'test-owner', 'mac1': 'x', 'mac2': 'y'},
                          {'name': 'r1', 'connectable': True, 'connected': False,
                           'autoReconnect': False, 'pairedBy': 'test-owner', 'mac1': 'z'}],
                'power': {'schema': 1, 'cpuMhz': roles.CLOCKS[key][-1], 'mode': 0,
                          'activeMhz': roles.CLOCKS[key][-1], 'idleMhz': roles.CLOCKS[key][0],
                          'supportedCpuMhz': roles.CLOCKS[key], 'powerSaveMinutes': 10}}
        self.original = copy.deepcopy(self.state)

    def step(self, name, fn):
        return fn()

    def save(self):
        pass

    def command(self, key, command, **kwargs):
        self.commands.append((key, command))
        s = self.state[key]
        parts = command.split()
        if command == 'bleprobe status':
            return '{"schema":1,"bridge":"board-gatt-v1","op":"status","ok":true,"connected":false,"subscribed":false}'
        if command == 'closeg2':
            s['g2'] = 'idle' if s['g2'] != 'off' else 'off'
            return 'G2: disconnected'
        if command == 'bleprobe disconnect' or command == 'ringdisconnect':
            return 'OK'
        if command == 'g2status':
            return f"state={s['g2']} L=down R=down"
        if command == 'blemode':
            return 'BLE mode: ' + s['mode']
        if command == 'loglink':
            return 'loglink: ' + s['loglink'].upper()
        if parts[0] == 'loglink':
            s['loglink'] = parts[1]
        elif parts[0] == 'bleautoreconnect':
            if self.restore_fail == key and parts[2] == 'on':
                raise RuntimeError('injected restoration failure')
            next(p for p in s['peers'] if p['name'] == parts[1])['autoReconnect'] = parts[2] == 'on'
        elif command == 'g2deinit':
            s['g2'] = 'off'
        elif command == 'g2init':
            s['g2'] = 'idle'
        elif parts[0] == 'blemode':
            s['mode'] = parts[1]
            if parts[1] == 'client':
                s['ble']['initialized'] = False
        elif command == 'openble':
            s['mode'] = 'server'
            s['ble']['initialized'] = True
        elif command == 'closeble':
            s['mode'] = 'client'
            s['ble']['initialized'] = False
        elif parts[0] == 'cpufreq':
            s['power']['cpuMhz'] = int(parts[1])
        else:
            raise AssertionError('Unexpected command ' + command)
        return 'OK'

    def json_command(self, key, command, field, **kwargs):
        s = self.state[key]
        if command == 'bleinfo json':
            value = s['ble']
        elif command == 'blepeers json':
            value = {'peers': s['peers']}
        elif command == 'ringstatus json':
            value = {'connectPending': False, 'connected': False}
        elif command == 'power json':
            value = s['power']
        elif command == 'espnowstatus json':
            value = {'initialized': True, 'mac': '00:01:02:03:04:05'}
        else:
            raise AssertionError(command)
        return copy.deepcopy(value)


class RolesTests(unittest.TestCase):
    def run_case(self, runner, fail=False, clocks=True):
        async def fake_probe(*args, **kwargs):
            runner.probe_count += 1
            if fail:
                raise RuntimeError('injected probe failure')
            return {'status': 'passed'}
        with patch.object(roles, 'run_probe', fake_probe):
            roles.execute(runner, {}, clocks=clocks)

    def test_success_preserves_enabled_auto_flags_and_clock(self):
        runner = FakeRunner()
        self.run_case(runner)
        self.assertEqual(runner.state, runner.original)
        self.assertEqual(runner.probe_count, 2)
        self.assertEqual(runner.results['status'], 'passed')
        for key, clocks in roles.CLOCKS.items():
            for mhz in clocks:
                self.assertIn((key, f'cpufreq {mhz}'), runner.commands)

    def test_probe_failure_still_restores_both(self):
        runner = FakeRunner()
        with self.assertRaisesRegex(RuntimeError, 'probe failure'):
            self.run_case(runner, fail=True)
        self.assertEqual(runner.state, runner.original)
        self.assertEqual(runner.results['status'], 'failed')

    def test_one_restore_failure_does_not_skip_other_board(self):
        runner = FakeRunner()
        runner.restore_fail = 's3'
        with self.assertRaisesRegex(Exception, 'restorations failed'):
            self.run_case(runner)
        self.assertEqual(runner.state['p4'], runner.original['p4'])
        self.assertEqual(len(runner.results['restorationErrors']), 1)
        self.assertEqual(runner.state['s3']['loglink'], 'off')

    def test_live_peer_rejected_before_settings_changes(self):
        runner = FakeRunner()
        runner.state['s3']['peers'][0]['connected'] = True
        original = copy.deepcopy(runner.state)
        with self.assertRaisesRegex(Exception, 'live accessory'):
            self.run_case(runner)
        self.assertEqual(runner.state, original)
        self.assertFalse(any(c.startswith(('bleautoreconnect ', 'cpufreq ', 'loglink ')) for _, c in runner.commands))

    def test_cancel_precedes_single_deinit(self):
        runner = FakeRunner()
        test = roles.BoardRoles(runner)
        test.stop_client('p4')
        sent = [c for key, c in runner.commands]
        self.assertLess(sent.index('ringdisconnect'), sent.index('closeg2'))
        self.assertLess(sent.index('closeg2'), sent.index('g2deinit'))
        self.assertEqual(sent.count('g2deinit'), 1)
        self.assertEqual(runner.state['p4']['g2'], 'off')

    def test_cancel_wait_is_bounded_and_does_not_force_deinit(self):
        runner = FakeRunner()
        test = roles.BoardRoles(runner)
        with patch.object(test, 'g2', return_value=('scanning', 'down', 'down')), \
             patch.object(roles.time, 'monotonic', side_effect=[0, 13]):
            with self.assertRaisesRegex(Exception, 'bounded wait'):
                test.stop_client('p4')
        self.assertNotIn(('p4', 'g2deinit'), runner.commands)

    def test_lazy_empty_peer_registration_is_allowed(self):
        saved = roles.peer_config([{'name': 'phone', 'autoReconnect': False}])
        current = [{'name': 'phone', 'autoReconnect': False},
                   {'name': 'g2-glasses', 'autoReconnect': False, 'connected': False,
                    'mac1': '', 'mac2': '', 'pairedBy': ''}]
        self.assertEqual(roles.verify_restored_peers(saved, current), ['g2-glasses'])
        for changed in ('mac1', 'mac2', 'pairedBy', 'autoReconnect', 'connected'):
            modified = copy.deepcopy(current)
            modified[1][changed] = True if changed in ('autoReconnect', 'connected') else 'nonempty'
            with self.subTest(changed=changed), self.assertRaisesRegex(Exception, 'empty default'):
                roles.verify_restored_peers(saved, modified)

    def test_saved_peer_changes_and_disappearance_are_rejected(self):
        saved = roles.peer_config([{'name': 'phone', 'autoReconnect': False}])
        with self.assertRaisesRegex(Exception, 'disappeared'):
            roles.verify_restored_peers(saved, [])
        with self.assertRaisesRegex(Exception, 'differs'):
            roles.verify_restored_peers(saved, [{'name': 'phone', 'autoReconnect': True}])

    def test_ownerless_auto_peer_rejected_before_changes(self):
        runner = FakeRunner()
        runner.state['p4']['peers'][0]['pairedBy'] = ''
        original = copy.deepcopy(runner.state)
        with self.assertRaisesRegex(Exception, 'ownerless'):
            self.run_case(runner)
        self.assertEqual(runner.state, original)


if __name__ == '__main__':
    unittest.main()
