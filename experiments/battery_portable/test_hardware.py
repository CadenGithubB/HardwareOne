#!/usr/bin/env python3
"""Explicit board-only battery qualification. Never flashes or provisions accounts.

Uses existing private credentials and reciprocal mesh pairing. An optional HTTP
check temporarily starts the existing isolated P4 AP fixture and joins it with
S3; it never uses the Mac's network or Bluetooth. Reboot after HTTP to restore
runtime radio state. Only the coordinator may own the serial ports.
"""
import argparse
import base64
import contextlib
import datetime as dt
import json
import math
from pathlib import Path
import secrets
import sys
import time
from types import SimpleNamespace
import urllib.parse

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent / 'p4_ble_roles'), str(HERE.parent / 'p4_connectivity'), str(HERE.parent / 'p4_mesh')]
from board_control import RolesConsole
from console import extract_json_objects
from test_mesh import MeshRunner, command_token, find_object, require
from test_http_s3 import HttpS3Probe, private_encodings, require_denied, validate_reply


def validate_battery(value):
    require(value.get('schema') == 1 and value.get('backend') == 'adc', 'Wrong battery backend')
    require(value.get('voltageAvailable') is True and value.get('voltageValid') is True, 'No valid ADC voltage')
    require(value.get('voltageCalibrated') is True, 'No P4 eFuse calibration')
    require(value.get('adcPin') == 49 and abs(value.get('divider', 0) - 1332/332) < 0.0001, 'Wrong board wiring')
    require(isinstance(value.get('voltage'), (float, int)) and math.isfinite(value['voltage']) and 0 <= value['voltage'] <= 6, 'Invalid terminal voltage')
    require(value.get('presenceKnown') is False and value.get('present') is None, 'Claimed unsupported cell presence')
    require(value.get('chargingKnown') is False and value.get('charging') is None, 'Claimed unsupported charging state')
    require(value.get('usbKnown') is False and value.get('usbPresent') is None, 'Claimed unsupported USB state')
    require(value.get('lastError') == 0 and value.get('stale') is False, 'ADC error/stale result')
    if value.get('percentageValid'):
        require(value.get('percentageEstimated') is True and value.get('percentageSource') == 'voltage', 'Unmarked SOC estimate')
        require(0 <= value['percentage'] <= 100, 'SOC estimate outside bounds')
    else:
        require(value.get('percentage') is None, 'Invalid SOC must be null')
    return value


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--credentials', required=True, type=Path)
    p.add_argument('--p4-port', required=True)
    p.add_argument('--s3-port', required=True)
    p.add_argument('--p4-mac', required=True)
    p.add_argument('--s3-mac', required=True)
    p.add_argument('--run-root', type=Path, default=HERE / 'private/hardware')
    p.add_argument('--http', action='store_true')
    args = p.parse_args()
    credentials = json.loads(args.credentials.read_text())
    private_values = private_encodings(credentials)
    run_id = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + secrets.token_hex(4)
    run = args.run_root / run_id
    run.mkdir(parents=True, mode=0o700)
    config = {key: {'port': getattr(args, key+'_port'), 'mac': getattr(args, key+'_mac').lower(), 'name': 'HW1_'+key.upper()}
              for key in ('p4', 's3')}
    options = SimpleNamespace(configure=False, pair='preserve', initiator='p4', rekey=False, reopen=False)
    with contextlib.ExitStack() as stack:
        boards = {key: stack.enter_context(RolesConsole(spec['port'], run/(key+'.log'), secrets=private_values, completion='hardwareone'))
                  for key, spec in config.items()}
        # USB attach can reset either board; let startup finish before any input.
        time.sleep(9)
        runner = MeshRunner(boards, config, credentials, run, run_id, options)
        runner.results['scope'] = 'P4 shared battery ADC integration, native S3 unavailable-backend compatibility, existing encrypted mesh and optional board-only HTTP'
        runner.results['physical_battery_presence'] = 'not confirmed by this test; measurement is BAT node'
        clocks = {}
        http_started = False
        probe = None
        def battery():
            return validate_battery(find_object(boards['p4'].command('batterystatus json', timeout=25), 'backend'))
        try:
            for key, board in boards.items():
                runner.login(key)
                require('loglink ON' in board.command('loglink on'), 'Serialized log routing unavailable')
                runner.step(key+'_identity', lambda key=key: runner.inspect(key))
                power = find_object(board.command('power json'), 'cpuMhz')
                clocks[key] = power['cpuMhz']
                runner.step(key+'_preserved_pair', lambda key=key: {'registry': runner.json_command(key, 'espnowlist', 'devices')})
            runner.step('p4_initial_battery', battery)
            def repeated():
                samples = []
                for _ in range(12):
                    samples.append(battery())
                    time.sleep(1.1)
                require(any(b['lastReadMsAgo'] < a['lastReadMsAgo'] for a,b in zip(samples,samples[1:])), 'Periodic sampler did not refresh')
                return {'samples': samples, 'voltage_min': min(x['voltage'] for x in samples), 'voltage_max': max(x['voltage'] for x in samples)}
            runner.step('p4_periodic_samples', repeated)
            def recalibrate():
                for _ in range(3):
                    reply = boards['p4'].command('batterycalibrate', timeout=30)
                    require('reloaded and reading refreshed' in reply, 'ADC recalibration failed')
                    battery()
                return {'cycles': 3, 'final': battery()}
            runner.step('p4_recalibration', recalibrate)
            def clock_samples():
                samples = []
                for clock in (100, 200, 400):
                    boards['p4'].command('cpufreq '+str(clock))
                    require(find_object(boards['p4'].command('power json'), 'cpuMhz')['cpuMhz'] == clock, 'P4 clock did not change')
                    boards['p4'].command('batterycalibrate')
                    samples.append({'cpu_mhz':clock, 'battery':battery()})
                return {'samples':samples}
            runner.step('p4_clock_readings', clock_samples)
            def disabled():
                reply = boards['s3'].command('batterystatus json')
                require('Unknown command' in reply or 'not available' in reply.lower(), 'S3 disabled profile unexpectedly exposes battery command')
                return {'monitor_compiled_out':True, 'physical_divider_available':False}
            runner.step('s3_disabled_profile', disabled)
            runner.step('preserved_mesh_sessions', runner.pair)
            for sender, receiver in (('p4','s3'),('s3','p4')):
                runner.step(sender+'_encrypted_text', lambda sender=sender, receiver=receiver: runner.text(sender, receiver, 401, phase='battery'))
            if args.http:
                require(find_object(boards['p4'].command('httpstatus json'), 'running').get('running') is False, 'Pre-existing HTTP server must be preserved')
                http_started = True  # AP may start even if the command later fails.
                ssid = credentials.get('ap_ssid', 'HW1_P4_TEST')
                ap = find_object(boards['p4'].command('probeap '+command_token(ssid)+' '+command_token(credentials['ap_password']), timeout=45), 'ap')
                require(ap.get('ap') and ap.get('http') and ap.get('channel') == 6, 'P4 HTTP fixture startup failed')
                http_started = True
                probe = HttpS3Probe(boards['s3'], credentials)
                runner.step('s3_http_join', probe.join)
                def request(path, fields=None):
                    method = 'POST' if fields is not None else 'GET'
                    arg = 'request '+method+' '+path
                    if fields is not None:
                        arg += ' '+base64.b64encode(urllib.parse.urlencode(fields).encode()).decode()
                    response = probe.rpc(arg)
                    require(response.get('connected') is True, 'S3 Wi-Fi disconnected during HTTP test')
                    reply = validate_reply(response)
                    probe.evidence.append({'path':path,'method':method,**reply.metadata})
                    return reply
                def http_check():
                    require(probe.rpc('clear').get('cookiePresent') is False, 'HTTP session did not clear')
                    require_denied(request('/api/battery/status'))
                    login = request('/login', {k:credentials[k] for k in ('username','password')})
                    require(login.status == 303 and login.metadata.get('cookiePresent'), 'HTTP cookie login failed')
                    page = request('/battery')
                    require(page.status == 200 and page.metadata.get('htmlOpen') and page.metadata.get('htmlClose'), 'Battery page incomplete')
                    data = request('/api/battery/status')
                    require(data.status == 200, 'Battery API failed')
                    document = validate_battery(json.loads(data.text()))
                    request('/logout')
                    require_denied(request('/api/battery/status'))
                    require(probe.rpc('clear').get('cookiePresent') is False, 'HTTP cookie retained')
                    return {'battery':document,'requests':probe.evidence,'protected_api':True,'logout':True}
                runner.step('p4_battery_http', http_check)
            runner.results['status'] = 'passed'
        except Exception as exc:
            runner.results.update(status='failed', error=runner.safe(type(exc).__name__+': '+str(exc)))
        finally:
            cleanup=[]
            for key, clock in clocks.items():
                try:
                    boards[key].command('cpufreq '+str(clock))
                    require(find_object(boards[key].command('power json'),'cpuMhz')['cpuMhz']==clock, 'Clock restore mismatch')
                except Exception as exc: cleanup.append(key+' clock: '+runner.safe(str(exc)))
            if probe is not None and not boards['s3'].fatal_console:
                try:
                    probe.rpc('request GET /logout')
                    require(probe.rpc('clear').get('cookiePresent') is False, 'HTTP cookie retained during cleanup')
                except Exception as exc: cleanup.append('HTTP session cleanup: '+runner.safe(str(exc)))
            if http_started:
                try: boards['p4'].command('closehttp')
                except Exception as exc: cleanup.append('HTTP stop: '+runner.safe(str(exc)))
            for key, board in boards.items():
                try: board.command('loglink off')
                except Exception as exc: cleanup.append(key+' loglink: '+runner.safe(str(exc)))
            runner.results['cleanup_errors']=cleanup
            runner.results['reboot_required_for_runtime_radio_restore']=http_started
            if cleanup: runner.results['status']='failed'
            runner.save()
        print(json.dumps({'status':runner.results['status'],'checks':len(runner.results['checks']),'result':str(run/'results.json')}),flush=True)
        return 0 if runner.results['status']=='passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
