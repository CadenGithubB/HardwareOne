#!/usr/bin/env python3
"""Private-credential, redacted serial coordinator for the connectivity test.

Explicit opt-in CLI. JSON lines on stdin: setup, command, mesh, or exit.
Keeps both serial ports open so browser/BLE tests do not trigger extra reboots.
Uses existing mesh pairing; never repairs it implicitly.
"""
import contextlib
import asyncio
import base64
import datetime as dt
import json
from pathlib import Path
import secrets
import sys
import time
import urllib.parse

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent/'p4_mesh'))
from connectivity_redaction import ConnectivityConsole
from test_mesh import MeshRunner, DEFAULT_BOARDS, parse_args, command_token


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
        boards = {key:stack.enter_context(ConnectivityConsole(spec['port'],run/(key+'.log'),
                      secrets=private_values,completion='hardwareone')) for key,spec in config.items()}
        time.sleep(9)
        runner = MeshRunner(boards,config,credentials,run,run_id,parse_args([]))
        for key in boards: runner.login(key)
        print(json.dumps({'ready':True,'logs':str(run)}),flush=True)
        records = []
        for line in sys.stdin:
            try:
                job = json.loads(line)
                action = job.get('action')
                if action == 'exit': break
                if action == 'setup':
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
                             on_connected=connected_probe))
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
                             on_cycle=http_cycle if job.get('mesh',False) else None)
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
