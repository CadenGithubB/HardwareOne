#!/usr/bin/env python3
"""Private-credential serial coordinator for mutually exclusive BLE role tests.

Explicit opt-in CLI. JSON lines on stdin: setclock, setup, command, mesh, ble, http, or exit.
Keeps both serial ports open so browser/BLE tests do not trigger extra reboots.
Uses existing mesh pairing; never repairs it implicitly.
"""
import contextlib
import asyncio
import base64
import datetime as dt
import json
import re
from pathlib import Path
import secrets
import sys
import time
import urllib.parse

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent/'p4_mesh'))
sys.path.insert(0, str(HERE.parent/'p4_connectivity'))
from connectivity_redaction import ConnectivityConsole
from console import ANSI, extract_json_objects
from test_mesh import MeshRunner, DEFAULT_BOARDS, parse_args, command_token


class RoleGuardError(RuntimeError):
    """A fixture could interfere with the production BLE owner."""


LIFECYCLE_COMMANDS = frozenset({
    'openble', 'closeble', 'blemode', 'g2init', 'g2deinit', 'openg2',
    'closeg2', 'g2scan', 'g2recover', 'ringconnect', 'ringdisconnect',
})
_G2_STATUS = re.compile(r'(?m)^(?:\$ )*(?:\[(?:serial|uart)[^\]\r\n]*\]\s*)?'
                        r'(?:OK: )?state=(\w+) L=(up|down) R=(up|down)\b')


def command_parts(command):
    # One direct command per request keeps lifecycle interception unambiguous.
    # This coordinator is a bounded test interface, not the complete CLI parser.
    if not isinstance(command, str) or not command.strip() or any(
            char in command for char in ('\r', '\n', '\x00', ';', '&', '|')):
        raise RoleGuardError('Use one direct command without separators')
    parts = command.strip().split()
    if not re.fullmatch(r'[a-zA-Z][a-zA-Z0-9_-]*', parts[0]):
        raise RoleGuardError('Command name must be an unquoted direct verb')
    return [part.lower() for part in parts]


def probe_reply(output, expected_op):
    matches = [obj for obj in extract_json_objects(output)
               if obj.get('schema') == 1 and obj.get('bridge') in
               ('board-gatt-v1', 's3-gatt-v1')]
    if len(matches) != 1 or matches[0].get('ok') is not True or matches[0].get('op') != expected_op:
        raise RoleGuardError('BLE test bridge did not acknowledge retirement')
    return matches[0]


class RolesConsole(ConnectivityConsole):
    """Retire the diagnostic central before production role/owner changes.

    This guard covers this coordinator's serial commands only. Do not change
    roles through HTTP, BLE, OLED, or another console while a probe is active.
    Existing automatic G2/R1 reconnect preferences must remain disabled during
    diagnostic-central tests; status checks cannot atomically exclude them.
    """
    def _retire_probe(self):
        closed = probe_reply(super().command('bleprobe disconnect', timeout=15), 'disconnect')
        status = probe_reply(super().command('bleprobe status', timeout=15), 'status')
        if closed.get('connected') is not False or status.get('connected') is not False or status.get('subscribed') is not False:
            raise RoleGuardError('BLE test bridge is not positively disconnected')

    def _require_central_idle(self):
        # There is no G2 JSON CLI status. This exact source format reports off
        # only when the actual client state is absent or uninitialized.
        output = super().command('g2status', timeout=15)
        states = _G2_STATUS.findall(ANSI.sub('', output))
        if len(states) != 1 or states[0] != ('off', 'down', 'down'):
            raise RoleGuardError('G2 client must be off before using the diagnostic BLE central')
        rings = [obj for obj in extract_json_objects(super().command('ringstatus json', timeout=15))
                 if obj.get('schema') == 1 and 'connectPending' in obj]
        if len(rings) != 1 or rings[0].get('connected') is not False or rings[0].get('connectPending') is not False:
            raise RoleGuardError('R1 client must be disconnected with no connect pending')

    def command(self, text, *, timeout=65):
        parts = command_parts(text)
        with self._transaction_lock:
            if parts[0] in LIFECYCLE_COMMANDS:
                self._retire_probe()
            if parts[0] == 'bleprobe' and len(parts) > 1 and parts[1] in ('scan', 'connect'):
                self._require_central_idle()
            return super().command(text, timeout=timeout)


def local_offset_minutes():
    return int(dt.datetime.now().astimezone().utcoffset().total_seconds() // 60)


def set_clock(board, offset, *, epoch=None):
    if type(offset) is not int or not -720 <= offset <= 840:
        raise ValueError('tzoffsetminutes must be an integer from -720 to 840')
    epoch = int(time.time()) if epoch is None else epoch
    if type(epoch) is not int or not 1577836800 <= epoch < 4102444800:
        raise ValueError('Clock epoch must be in 2020-2099')
    replies = [board.command(f'tzoffsetminutes {offset}', timeout=30),
               board.command(f'timeset {epoch}', timeout=30)]
    if f'Timezone offset set to {offset} minutes' not in replies[0] or not re.search(
            r'(?m)^(?:\$ )*(?:\[(?:serial|uart)[^\]\r\n]*\]\s*)?OK\s*$', replies[1]):
        raise RoleGuardError('Clock commands were not acknowledged')
    return {'epoch': epoch, 'tzoffsetminutes': offset, 'acknowledged': True}


def raw_ble_secret(value):
    # cmd_blesecret consumes the entire trimmed argument literally. Unlike
    # token-parsing commands, added quote characters become part of the key.
    if (not isinstance(value, str) or not 10 <= len(value) <= 128 or value != value.strip()
            or any(ord(char) < 33 or ord(char) > 126 for char in value)
            or any(char in value for char in ('"', "'", "\\", ";", "|", "&", "<", ">", "`"))):
        raise ValueError('BLE test secret must be a printable unquoted token without command separators')
    return value


def main():
    credentials = json.loads((HERE/'private/credentials.json').read_text())
    run_id = secrets.token_hex(4)
    run = HERE/'private/serial-runs'/(dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+run_id)
    run.mkdir(parents=True, mode=0o700)
    private_values = [v for k,v in credentials.items() if k != 'mesh_label' and isinstance(v,str)]
    private_values += [base64.b64encode(v.encode()).decode() for v in private_values]
    login_form = urllib.parse.urlencode({k:credentials[k] for k in ('username','password')})
    private_values += [login_form, base64.b64encode(login_form.encode()).decode()]
    config = {key:dict(spec) for key,spec in DEFAULT_BOARDS.items()}
    with contextlib.ExitStack() as stack:
        boards = {key:stack.enter_context(RolesConsole(spec['port'],run/(key+'.log'),
                      secrets=private_values,completion='hardwareone')) for key,spec in config.items()}
        time.sleep(9)
        runner = MeshRunner(boards,config,credentials,run,run_id,parse_args([]))
        for key in boards:
            runner.login(key)
            # Route IDF and application logs through the same queue. Separate
            # writers can otherwise split a command's completion line mid-byte.
            linked = boards[key].command('loglink on', timeout=30)
            if 'loglink ON' not in linked:
                raise RoleGuardError('Serial log serialization was not acknowledged')
        print(json.dumps({'ready':True,'logs':str(run)}),flush=True)
        records = []
        for line in sys.stdin:
            try:
                job = json.loads(line)
                action = job.get('action')
                if action == 'exit': break
                if action == 'setclock':
                    keys = [job['board']] if job.get('board') else list(boards)
                    offset = job.get('tzoffsetminutes', local_offset_minutes())
                    result = {'clocks': {key: set_clock(boards[key], offset) for key in keys}}
                elif action == 'setup':
                    key = job['board']
                    responses = []
                    for command in (
                        f'blename HW1_{key.upper()}_BLE',
                        f'blesecret {raw_ble_secret(credentials["ble_secret"])}',
                        'blesecure on', 'openble',
                        f'probeap HW1_{key.upper()}_TEST {command_token(credentials["ap_password"])}',
                        'blestatus json','httpstatus json'):
                        responses.append(boards[key].redact(runner.command(key,command,timeout=45)))
                    result = {'setup':key,'responses':responses}
                elif action == 'command':
                    key = job['board']
                    result = {'board':key,'response':boards[key].redact(runner.command(
                              key,job['command'],timeout=job.get('timeout',30),allow_error=True))}
                elif action == 'mesh':
                    result = {'session':runner.sessions(), 'messages':[]}
                    for index in range(min(max(int(job.get('count',1)),1),100)):
                        for sender,receiver in (('p4','s3'),('s3','p4')):
                            result['messages'].append(runner.text(sender,receiver,401,phase='coexist-'+str(index)))
                elif action == 'ble':
                    from test_ble import run_probe
                    client_key = job.get('client','s3')
                    target_key = 'p4' if client_key == 's3' else 's3'
                    async def connected_probe(connection, cycle):
                        if not job.get('mesh', False): return {'mesh': 'not requested'}
                        session = runner.sessions()
                        messages = []
                        for sender, receiver in (('p4','s3'), ('s3','p4')):
                            messages.append(runner.text(sender,receiver,401,phase='ble-coexist-'+str(cycle)))
                        return {'session':session, 'messages':messages}
                    result = asyncio.run(run_probe(boards[client_key], credentials,
                             expected_name=f'HW1_{target_key.upper()}_BLE', expected_mac=config[target_key]['mac'],
                             expected_ble_mac=job.get('ble_mac'),
                             on_connected=connected_probe, timeout=job.get('timeout',65),
                             run_root=HERE/'private/ble-runs'))
                elif action == 'http':
                    from test_http_s3 import run_probe_http
                    coexistence_messages = []
                    def http_cycle(index):
                        if not job.get('mesh',False): return {'mesh':'not requested'}
                        evidence = {'session':runner.sessions(), 'messages':[
                            runner.text(sender,receiver,401,phase='http-coexist-'+str(index))
                            for sender,receiver in (('p4','s3'),('s3','p4'))]}
                        coexistence_messages.append(evidence)
                        return evidence
                    result = run_probe_http(boards['s3'],credentials,
                             duration=job.get('duration',0),join=job.get('join',True),
                             on_cycle=http_cycle if job.get('mesh',False) else None,
                             run_root=HERE/'private/http-runs')
                    result['coexistenceMessages'] = coexistence_messages
                else:
                    raise ValueError('Unknown action')
                def sanitized(value):
                    if isinstance(value, str):
                        for board in boards.values(): value = board.redact(value)
                        return value
                    if isinstance(value, dict):
                        return {sanitized(str(key)):sanitized(item) for key,item in value.items()}
                    if isinstance(value, (tuple,list)):
                        return [sanitized(item) for item in value]
                    return value
                result = sanitized(result)
                records.append({'action':action,'result':result})
                saved = json.dumps(sanitized(records),indent=2)
                result_path = run/'results.json'
                result_path.write_text(saved+'\n')
                result_path.chmod(0o600)
                print(json.dumps(result),flush=True)
            except Exception as exc:
                message = str(exc)
                for board in boards.values(): message = board.redact(message)
                fatal = [key for key,board in boards.items() if board.fatal_console]
                print(json.dumps({'error':type(exc).__name__+': '+message,
                                  'fatalConsole':bool(fatal), 'restartRequired':bool(fatal),
                                  'blockedBoards':fatal}),flush=True)
                if fatal: break  # ExitStack closes ports; no more commands follow.
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
