#!/usr/bin/env python3
"""Focused real-device recovery checks, independent of the file-transfer matrix.

Uses the same private credentials and default ports as test_mesh.py. Preserves
existing pairing. Serial reopen may reboot both boards. No actions on import.
Run only after closing all other clients of the two serial ports.
"""
import json, sys, time, contextlib, datetime, secrets
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from console import MeshConsole
from test_mesh import MeshRunner,parse_args,DEFAULT_BOARDS

def main():
    base=Path(__file__).resolve().parent/'private'
    creds=json.loads((base/'credentials.json').read_text())
    args=parse_args(['--pair','preserve'])
    run_id=secrets.token_hex(4)
    run=base/'test-runs'/('recovery-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+run_id)
    run.mkdir()
    config={key:dict(spec) for key,spec in DEFAULT_BOARDS.items()}
    with contextlib.ExitStack() as stack:
        boards={key:stack.enter_context(MeshConsole(spec['port'],run/(key+'.log'),completion='hardwareone',secrets=[creds['username'],creds['password'],creds['mesh_passphrase']])) for key,spec in config.items()}
        time.sleep(9)
        r=MeshRunner(boards,config,creds,run,run_id,args)
        failed=False
        try:
            for key in boards:
                r.step('login_'+key,lambda key=key:r.login(key))
                r.step('inspect_'+key,lambda key=key:r.inspect(key))
            r.step('pairing',r.pair)
            r.step('active_session',r.sessions)
            for phase,action in [('rekey',r.rekey),('reopen',r.reopen)]:
                try:
                    r.step(phase,action)
                    for sender,receiver in [('p4','s3'),('s3','p4')]:
                        r.step(phase+'_text_'+sender,lambda sender=sender,receiver=receiver:r.text(sender,receiver,401,phase=phase))
                except Exception as e:
                    print('FAIL',phase,type(e).__name__,r.safe(str(e)),flush=True)
                    failed=True
            for key in boards:
                r.step('statistics_end_'+key,lambda key=key:r.json_command(key,'espnowstats json','schema'))
        except Exception as e:
            print('FAIL',type(e).__name__,r.safe(str(e)),flush=True)
            failed=True
        r.results.update(status='failed' if failed else 'passed',scope='focused recovery investigation')
        r.save()
    print('RESULTS',run/'results.json',flush=True)
    return 1 if failed else 0

if __name__ == '__main__':
    raise SystemExit(main())
