#!/usr/bin/env python3
"""Opt-in USB microphone qualification through shipping HAL/recorder commands.

No flashing or provisioning. Existing accounts/settings are retained. Captured
WAVs and serial logs stay private. --tone-player /usr/bin/afplay emits a brief
997 Hz reference through the workstation speaker; no workstation mic is used.
"""
import argparse, array, base64, contextlib, datetime, hashlib, json, math
from pathlib import Path
import re, secrets, subprocess, sys, time, wave

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent / n) for n in ('p4_ble_roles', 'p4_connectivity', 'p4_mesh')]
from board_control import RolesConsole
from console import ANSI, ConsoleTimeout, extract_json_objects
from connectivity_redaction import ConnectivityConsoleFatal
from test_http_s3 import private_encodings
from test_mesh import command_token


def require(ok, message):
    if not ok: raise ValueError(message)



def file_chunk_pattern(path, offset):
    """Match a complete shipping fileread envelope for one immutable test range."""
    require(isinstance(path, str) and re.fullmatch(r'/recordings/rec_[0-9a-f]{16}\.wav', path),
            'Expected a test-owned recording path')
    require(type(offset) is int and offset >= 0, 'Invalid file offset')
    # The serializer emits this field order without whitespace. Match data
    # broadly enough to reject malformed base64 immediately instead of waiting
    # for a prompt. Wrong paths/offsets and incomplete envelopes never match.
    return re.compile(r'\{"success":true,"path":' + re.escape(json.dumps(path)) +
                      r',"size":[0-9]+,"offset":' + str(offset) +
                      r',"len":[0-9]+,"eof":(?:true|false),"enc":"[^"\r\n]*",'
                      r'"data":"[^"\r\n]*"\}')


def parse_file_chunk(reply, path, offset, total=None, chunk_size=512):
    """Validate identity, bounds and canonical base64; allow identical retries."""
    require(type(chunk_size) is int and 0 < chunk_size <= 512, 'Invalid chunk size')
    matches = list(file_chunk_pattern(path, offset).finditer(ANSI.sub('', reply)))
    require(matches, 'Missing complete matching file chunk')
    accepted = None
    for match in matches:
        value = json.loads(match.group(0))
        size, length = value['size'], value['len']
        require(type(size) is int and 44 < size <= 1024*1024, 'Invalid WAV size')
        require(total is None or size == total, 'File changed during read')
        require(value['enc'] == 'b64' and offset < size, 'Invalid file encoding/range')
        require(type(length) is int and length == min(chunk_size, size-offset), 'Malformed file length')
        try:
            chunk = base64.b64decode(value['data'], validate=True)
        except (ValueError, TypeError) as error:
            raise ValueError('Malformed file base64') from error
        require(len(chunk) == length and base64.b64encode(chunk).decode('ascii') == value['data'],
                'Malformed file chunk')
        require(value['eof'] is (offset+length == size), 'Bad EOF')
        current = (size, chunk)
        require(accepted is None or current == accepted, 'Conflicting duplicate file chunks')
        accepted = current
    return accepted


def read_owned_file(board, path, health_check, on_retry, *, chunk_size=512, wait=time.sleep):
    """Read only: bounded same-offset retry without a second whoami response.

    fileread closes the file before building its complete JSON result, and the
    command executor copies that result after the handler returns. Thus a full
    matching envelope is sufficient completion for this read. A delayed retry
    can only reread the same immutable range; later commands remain serialized
    by the executor and retain their ordinary whoami completion barrier.
    """
    file_chunk_pattern(path, 0)  # Reject unrelated paths before any device I/O.
    require(type(chunk_size) is int and 0 < chunk_size <= 512, 'Invalid chunk size')
    data = bytearray(); total = None
    while total is None or len(data) < total:
        offset = len(data)
        for attempt in range(4):
            if board.fatal_console:
                raise ConnectivityConsoleFatal('Console completion is unknown; close/reopen before file read')
            health_check()
            mark = board.send_line('fileread '+command_token(path)+' '+str(offset)+' '+str(chunk_size)+' b64')
            failure = None
            try:
                reply = board.read_until(file_chunk_pattern(path, offset), since=mark, timeout=10)
            except ConsoleTimeout as error:
                failure = error
            # Board failures never become retryable parsing/timeout failures.
            health_check()
            if failure is None:
                try:
                    new_total, chunk = parse_file_chunk(reply, path, offset, total, chunk_size)
                except ValueError as error:
                    failure = error
            wait(.08)
            if failure is None:
                total = new_total; data.extend(chunk)
                break
            if attempt == 3:
                raise failure
            on_retry()
    require(len(data) == total, 'File size mismatch')
    return data


def inspect_wav(path, rate, elapsed, tone=False, confirmed_window=0):
    with wave.open(str(path), 'rb') as f:
        require((f.getnchannels(), f.getsampwidth(), f.getframerate(), f.getcomptype()) == (1,2,rate,'NONE'), 'Wrong PCM WAV format')
        n = f.getnframes(); raw = f.readframes(n)
        require(len(raw) == n*2 and n > rate/4, 'Empty/truncated WAV')
    x = array.array('h', raw)
    if sys.byteorder != 'little': x.byteswap()
    rms = math.sqrt(sum(v*v for v in x)/n)
    clipped = sum(abs(v) >= 32760 for v in x)/n
    duration = n/rate
    require(max(.25, confirmed_window-.25) < duration < elapsed+1, 'Implausible capture duration/sample loss')
    require(len(set(x)) > 20 and rms > 1, 'No dynamic microphone signal')
    require(clipped < .01, 'Audio clipping exceeds 1%')
    result = {'sha256':hashlib.sha256(path.read_bytes()).hexdigest(), 'bytes':path.stat().st_size,
              'rate':rate,'channels':1,'bits':16,'samples':n,'duration_s':duration,
              'wall_s':elapsed,'confirmed_window_s':confirmed_window,'rms':rms,'peak':max(abs(v) for v in x),'clipped_fraction':clipped}
    if tone:
        # Search short windows because startup and command latency surround the
        # reference tone. A sinusoid at the expected acoustic frequency proves
        # capture is not merely nonzero DMA garbage or a wrong-clock waveform.
        width = min(4096, n); best = None
        for offset in range(0, n-width+1, max(1,width//2)):
            segment = x[offset:offset+width]
            mean = sum(segment)/width
            values = [(v-mean)*(.5-.5*math.cos(2*math.pi*i/(width-1))) for i,v in enumerate(segment)]
            energies = []
            for freq in range(900,1101,2):
                coefficient = 2*math.cos(2*math.pi*freq/rate)
                a=b=0.0
                for v in values: a,b = v+coefficient*a-b,a
                energies.append((a*a+b*b-coefficient*a*b,freq))
            energy,freq=max(energies)
            purity=energy/(sum(v*v for v in values)*width+1)
            if best is None or purity > best['purity']: best={'frequency_hz':freq,'purity':purity}
        result['reference_tone']=best
        require(best and abs(best['frequency_hz']-997)<=8 and best['purity']>.08, '997 Hz acoustic reference not detected')
    return result


def make_tone(path):
    rate=48000
    samples=array.array('h', (int(5000*math.sin(2*math.pi*997*i/rate)) for i in range(rate*8)))
    if sys.byteorder != 'little': samples.byteswap()
    with wave.open(str(path),'wb') as f:
        f.setnchannels(1);f.setsampwidth(2);f.setframerate(rate);f.writeframes(samples.tobytes())


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--credentials',type=Path,required=True)
    p.add_argument('--board',choices=('p4','s3'),required=True)
    p.add_argument('--rates',type=int,nargs='+',choices=(8000,16000,48000),default=(16000,8000,48000),help='PCM rates to qualify before coexistence checks')
    scope=p.add_mutually_exclusive_group()
    scope.add_argument('--coexist-only',action='store_true',help='Resume camera/microphone coexistence and source checks without rate/restart tests')
    scope.add_argument('--skip-coexist',action='store_true',help='Run only selected rates and restart tests')
    p.add_argument('--port',required=True);p.add_argument('--mac',required=True,help='Existing logical radio MAC')
    p.add_argument('--tone-player',type=Path)
    p.add_argument('--tone-volume',type=int,choices=range(1,51),help='Temporarily use this macOS output volume only during each tone (1..50)')
    p.add_argument('--run-root',type=Path,default=HERE/'private/hardware')
    args=p.parse_args()
    if args.tone_volume and (not args.tone_player or str(args.tone_player) != '/usr/bin/afplay'):
        p.error('--tone-volume requires --tone-player /usr/bin/afplay')
    credentials=json.loads(args.credentials.read_text())
    run=args.run_root/(datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+args.board+'-'+secrets.token_hex(3))
    run.mkdir(parents=True,mode=0o700)
    result={'schema':1,'board':args.board,'scope':'coexist-only' if args.coexist_only else 'rates-only' if args.skip_coexist else 'full','checks':[],'restored':{},'transport_rereads':0}
    def save():
        out=run/'results.json';out.write_text(json.dumps(result,indent=2)+'\n');out.chmod(0o600)
    old={};owned={};loglinked=False
    with RolesConsole(args.port,run/'serial.log',secrets=private_encodings(credentials),completion='hardwareone',chunk_delay=.08) as b:
        def health_check():
            require(not re.search(r'Guru Meditation|panic\(|abort\(|Brownout',b.read_since(0)), 'Fatal board log')
        def cmd(text):
            output=b.command(text,timeout=65)
            time.sleep(.06)  # Let the USB prompt drain before the next command.
            health_check()
            return output
        def must(text,token):
            output=cmd(text);require(token in output,'Command failed: '+text);return output
        def obj(text,key):
            values=[v for v in extract_json_objects(cmd(text)) if key in v]
            require(len(values)==1,'Missing/ambiguous status: '+text);return values[0]
        def check(name,value):
            result['checks'].append({'name':name,'passed':True,'result':value});save()
            print(json.dumps({'board':args.board,'passed':name}),flush=True)
        def read_file(path):
            require(path in owned.values(), 'Refusing unrelated file transfer')
            def reread():
                result['transport_rereads'] += 1
            return read_owned_file(b, path, health_check, reread)
        def record(label,rate,coexist=False):
            token=secrets.token_hex(8)
            while token[:8]=='00000000' or token[8:]=='00000000': token=secrets.token_hex(8)
            path='/recordings/rec_'+token+'.wav';owned[token]=path
            player=None;volume_state=None
            try:
                if args.tone_player:
                    if args.tone_volume:
                        state=subprocess.check_output(['/usr/bin/osascript','-e','get volume settings'],text=True)
                        volume_state=(int(re.search(r'output volume:(\d+)',state).group(1)),re.search(r'output muted:(true|false)',state).group(1))
                        subprocess.run(['/usr/bin/osascript','-e','set volume output volume '+str(args.tone_volume)+' output muted false'],check=True,capture_output=True)
                    player=subprocess.Popen([str(args.tone_player),'-v','0.5',str(run/'reference.wav')],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
                    time.sleep(.2)
                    require(player.poll() is None,'Reference-tone player exited before recording')
                started=time.monotonic()
                must('micrecord startid '+token,'Recording '+token+' started')
                confirmed_start=time.monotonic()
                if coexist:
                    must('cameracapture','Captured frame:')
                    mesh=obj('espnowstatus json','pairedDevices')
                    require(mesh['initialized'] and mesh['pairedDevices']>0,'Mesh unavailable while recording')
                time.sleep(.65)
                confirmed_window=time.monotonic()-confirmed_start
                must('micrecord stopid '+token,'Recording '+token+' stopped — '+path)
                elapsed=time.monotonic()-started
                require(not obj('micread json','recordingState')['recording'],'Recorder still active')
            finally:
                try:
                    if player:
                        player.terminate();player.wait(timeout=5)
                finally:
                    if volume_state is not None:
                        subprocess.run(['/usr/bin/osascript','-e','set volume output volume '+str(volume_state[0])+' output muted '+volume_state[1]],check=True,capture_output=True)
            out=run/(label+'.wav');out.write_bytes(read_file(path));out.chmod(0o600)
            metrics=inspect_wav(out,rate,elapsed,bool(args.tone_player),confirmed_window)
            must('micdeleteid '+token+' '+command_token(Path(path).name),'Deleted recording '+token+': '+Path(path).name)
            del owned[token]
            check(label,metrics)
        try:
            time.sleep(9)
            cmd('login '+command_token(credentials['username'])+' '+command_token(credentials['password']))
            must('whoami','You are '+credentials['username']+' (admin)')
            must('loglink on','loglink ON');loglinked=True
            identity=obj('espnowstatus json','mac');require(identity['mac'].lower()==args.mac.lower(),'Wrong physical fixture')
            initial=obj('micread json','recordingState');require(not initial['recording'],'Recorder already owned')
            require(initial['pdmAvailable'],'Onboard PDM unavailable')
            old['enabled']=initial['enabled'];old['source']=re.search(r'preference=(auto|pdm|g2)',cmd('micsource')).group(1)
            old['camera']=obj('cameraread','backend');require(not old['camera']['enabled'],'Camera must start stopped for standalone mic test')
            # Start once without changing settings so lazy-loaded gain/rate are
            # read from persisted configuration, not pre-start cached defaults.
            must('openmic','Microphone started')
            old['rate']=int(re.search(r'Sample rate: (\d+)',cmd('micsamplerate')).group(1))
            old['gain']=int(re.search(r'Gain: (\d+)',cmd('micgain')).group(1))
            result['original']=old;save()
            require(obj('micread json','source')['source']=='pdm','PDM not selected')
            if args.tone_player: make_tone(run/'reference.wav')
            if not args.coexist_only:
                for rate in args.rates:
                    must('micsamplerate '+str(rate),'Sample rate set to')
                    record('pdm-'+str(rate),rate)
                must('micsamplerate 16000','Sample rate set to')
                for i in range(3):
                    must('closemic','Microphone stopped')
                    require(not obj('micread json','enabled')['enabled'],'Close did not stop capture')
                    must('openmic','Microphone started')
                    require(obj('micread json','connected')['connected'],'Reopen failed')
                check('three-restart-cycles',True)
            if not args.skip_coexist:
                must('micsamplerate 16000','Sample rate set to')
                must('opencamera','Camera started successfully')
                record('camera-and-mic',16000,True)
                must('closemic','Microphone stopped')
                must('cameracapture','Captured frame:')
                check('camera-survives-mic-stop',True)
                must('closecamera','Camera stopped')
                must('openmic','Microphone started')
                record('mic-after-camera-stop',16000)
                must('micsource g2',"preference set to 'g2'")
                require(obj('micread json','source')['source']=='pdm','Unavailable G2 fallback failed')
                must('micsource auto',"preference set to 'auto'")
                require(obj('micread json','source')['source']=='pdm','Auto did not select PDM')
                check('unavailable-g2-and-auto-local-fallback',True)
            result['status']='passed'
        except Exception as e:
            result['status']='failed';result['error']=b.redact(str(e))
        finally:
            def cleanup(name,action):
                try: action();result['restored'][name]=True
                except Exception as e: result['restored'][name]=b.redact(str(e));result['status']='failed'
                save()
            if old:
                cleanup('close-mic',lambda: must('closemic','Microphone stopped'))
                for token,path in tuple(owned.items()):
                    cleanup('discard-'+token,lambda t=token: must('micrecord stopid '+t+' discard','discarded'))
                if 'rate' in old: cleanup('rate',lambda: must('micsamplerate '+str(old['rate']),'Sample rate set to'))
                if 'source' in old: cleanup('source',lambda: must('micsource '+old['source'],'preference set to'))
                if 'camera' in old and not old['camera']['enabled']: cleanup('camera',lambda: must('closecamera','Camera stopped'))
                if 'rate' in old: cleanup('rate-readback',lambda: require('Sample rate: '+str(old['rate'])+' Hz' in cmd('micsamplerate'),'Rate not restored'))
                if 'source' in old: cleanup('source-readback',lambda: require('preference='+old['source']+',' in cmd('micsource'),'Source preference not restored'))
                if 'gain' in old: cleanup('gain-unchanged',lambda: require('Gain: '+str(old['gain'])+'%' in cmd('micgain'),'Gain changed'))
                if 'camera' in old: cleanup('camera-readback',lambda: require(obj('cameraread','backend')['enabled']==old['camera']['enabled'],'Camera state not restored'))
                if old.get('enabled'): cleanup('mic-enabled',lambda: must('openmic','Microphone started'))
                cleanup('mic-final-status',lambda: require(obj('micread json','enabled')['enabled']==old['enabled'],'Mic enable changed'))
            if loglinked: cleanup('loglink',lambda: cmd('loglink off'))
            save()
    print(json.dumps({'status':result['status'],'board':args.board,'checks':len(result['checks']),'error':result.get('error'),'run':str(run)}),flush=True)
    return 0 if result['status']=='passed' else 1

if __name__=='__main__': sys.exit(main())
