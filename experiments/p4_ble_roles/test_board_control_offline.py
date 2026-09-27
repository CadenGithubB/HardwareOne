"""Offline role guard checks; no serial ports are opened."""
import importlib.util
import json
from pathlib import Path
import threading
import unittest
from unittest.mock import patch

_SPEC = importlib.util.spec_from_file_location('roles_coordinator', Path(__file__).with_name('board_control.py'))
roles = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(roles)


def response(op, **fields):
    return json.dumps(dict(schema=1, bridge='board-gatt-v1', ok=True,
                          op=op, connected=False, **fields))


class GuardTests(unittest.TestCase):
    def setUp(self):
        self.board = roles.RolesConsole.__new__(roles.RolesConsole)
        self.board._transaction_lock = threading.RLock()

    def test_every_lifecycle_verb_retires_then_checks_before_execution(self):
        for verb in roles.LIFECYCLE_COMMANDS:
            calls = []
            def command(_board, text, **kwargs):
                calls.append(text)
                if text == 'bleprobe disconnect': return response('disconnect')
                if text == 'bleprobe status': return response('status', subscribed=False)
                return 'accepted'
            with patch.object(roles.ConnectivityConsole, 'command', command):
                self.assertEqual(self.board.command(verb + ' test'), 'accepted')
            self.assertEqual(calls, ['bleprobe disconnect', 'bleprobe status', verb + ' test'])

    def test_failed_retirement_does_not_send_lifecycle_command(self):
        with patch.object(roles.ConnectivityConsole, 'command', return_value=json.dumps(
                {'schema':1, 'bridge':'board-gatt-v1', 'ok':False})) as send:
            with self.assertRaises(roles.RoleGuardError): self.board.command('blemode client')
        self.assertEqual(send.call_count, 1)

    def test_missing_or_live_post_close_state_fails_closed(self):
        for status in (response('status'), response('status', subscribed=True),
                       response('status', subscribed=False).replace('"connected": false', '"connected": true')):
            with patch.object(roles.ConnectivityConsole, 'command', side_effect=[response('disconnect'), status]) as send:
                with self.assertRaises(roles.RoleGuardError): self.board.command('g2init')
            self.assertEqual(send.call_count, 2)

    def test_g2_idle_is_initialized_and_must_not_admit_probe(self):
        for output in ('state=idle L=down R=down mtu=L0/R0',
                       'state=connected L=up R=up mtu=L244/R244', 'Unknown command'):
            with patch.object(roles.ConnectivityConsole, 'command', return_value=output) as send:
                with self.assertRaises(roles.RoleGuardError): self.board.command('bleprobe connect 00:11:22:33:44:55')
            self.assertEqual(send.call_count, 1)

    def test_off_g2_and_inactive_ring_allow_scan_or_connect(self):
        for probe in ('bleprobe scan 5', 'bleprobe connect 00:11:22:33:44:55 0'):
            with patch.object(roles.ConnectivityConsole, 'command', side_effect=[
                    '$ state=off L=down R=down mtu=L0/R0\r\n',
                    '{"schema":1,"connected":false,"connectPending":false}', 'probe result']) as send:
                self.assertEqual(self.board.command(probe), 'probe result')
            self.assertEqual([x.args[0] for x in send.call_args_list], ['g2status','ringstatus json',probe])

    def test_actual_ok_prefixed_g2_reply_allows_probe(self):
        actual = ('OK: state=off L=down R=down mtu=L0/R0 batt=L-1/R-1 '
                  'tx=L0/R0 rx=L0/R0\n$ You are [REDACTED] (admin)\n$ ')
        for prefix in ('', '$ ', '[serial test@local] '):
            with patch.object(roles.ConnectivityConsole, 'command', side_effect=[
                    prefix + actual,
                    '{"schema":1,"connected":false,"connectPending":false}',
                    'probe result']) as send:
                self.assertEqual(self.board.command('bleprobe scan 5'), 'probe result')
            self.assertEqual(send.call_count, 3)

    def test_ok_prefix_does_not_admit_initialized_or_multiple_statuses(self):
        off = 'OK: state=off L=down R=down mtu=L0/R0\n'
        for reply in (
                'OK: state=idle L=down R=down mtu=L0/R0\n',
                'OK: state=connected L=up R=up mtu=L244/R244\n',
                off + off,
                off + 'state=idle L=down R=down mtu=L0/R0\n',
                'state=off L=down R=down mtu=L0/R0\n' + off):
            with patch.object(roles.ConnectivityConsole, 'command', return_value=reply) as send:
                with self.assertRaises(roles.RoleGuardError): self.board.command('bleprobe scan 5')
            self.assertEqual(send.call_count, 1)

    def test_pending_ring_blocks_probe(self):
        with patch.object(roles.ConnectivityConsole, 'command', side_effect=[
                'state=off L=down R=down', '{"schema":1,"connected":false,"connectPending":true}']) as send:
            with self.assertRaises(roles.RoleGuardError): self.board.command('bleprobe scan 5')
        self.assertEqual(send.call_count, 2)

    def test_compound_or_quoted_verbs_cannot_bypass_interception(self):
        with patch.object(roles.ConnectivityConsole, 'command') as send:
            for value in ('whoami; closeble', 'whoami && openble', 'whoami\nblemode client', '"closeble"', 'whoami | openble'):
                with self.assertRaises(roles.RoleGuardError): self.board.command(value)
            send.assert_not_called()

    def test_clock_uses_unquoted_epoch_and_actual_timezone_verb(self):
        with patch.object(self.board, 'command', side_effect=[
                'Timezone offset set to -240 minutes', '$ OK\r\nYou are test (admin)\r\n']) as send:
            result = roles.set_clock(self.board, -240, epoch=1790517600)
        self.assertEqual([x.args[0] for x in send.call_args_list], ['tzoffsetminutes -240','timeset 1790517600'])
        self.assertTrue(result['acknowledged'])
        for offset in (-721, 841, True, '-240'):
            with self.assertRaises(ValueError): roles.set_clock(self.board, offset)


if __name__ == '__main__':
    unittest.main()
