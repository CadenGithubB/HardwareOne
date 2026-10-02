#!/usr/bin/env python3
"""Explicit USB camera qualification; never flashes or provisions devices.

Uses existing credentials and pairing. Photographs and detailed logs remain in
ignored private run folders. Only unique test photos are removed from boards;
original camera settings are restored. Optional HTTP uses P4 AP and S3 client,
not the workstation network. Reboot both boards afterwards to retire that AP.
--board p4 or --board s3 opens only that USB console and skips interop checks.
Pass --image-python with a Pillow-enabled interpreter when the USB/IDF Python
has no Pillow (on this workstation, the bundled Codex dependency Python is
~/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3).
"""
import argparse
import base64
import contextlib
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time
from types import SimpleNamespace
import urllib.parse

HERE = Path(__file__).resolve().parent
sys.path[:0] = [str(HERE.parent / name) for name in ('p4_ble_roles', 'p4_connectivity', 'p4_mesh')]
from board_control import RolesConsole, command_parts
from console import extract_json_objects
from test_http_s3 import HttpS3Probe, private_encodings, require_denied, validate_reply
from test_mesh import MeshRunner, command_token, require, find_object


def jpeg_dimensions(data):
    require(data[:2] == b'\xff\xd8' and data[-2:] == b'\xff\xd9', 'Incomplete JPEG markers')
    pos = 2
    while pos + 4 < len(data):
        require(data[pos] == 255, 'Invalid JPEG marker')
        while pos < len(data) and data[pos] == 255:
            pos += 1
        require(pos + 2 < len(data), 'Truncated JPEG marker')
        marker = data[pos]
        pos += 1
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        size = int.from_bytes(data[pos:pos+2], 'big')
        require(size >= 2 and pos + size <= len(data), 'Invalid JPEG segment')
        if marker in (0xC0, 0xC1, 0xC2):
            require(size >= 8, 'Truncated JPEG geometry')
            return int.from_bytes(data[pos+5:pos+7], 'big'), int.from_bytes(data[pos+3:pos+5], 'big')
        require(marker != 0xDA, 'JPEG lacks geometry before scan')
        pos += size
    raise ValueError('JPEG lacks SOF geometry')


def jpeg_entropy_diagnostics(data):
    """Qualification heuristic for observed DMA corruption, not JPEG validity.

    Ignore segment/header bytes and inspect only SOS entropy spans, including
    stuffed bytes and restart markers. Resume segment parsing between scans so
    progressive/multiscan headers cannot produce a false zero-run anomaly.
    """
    require(data[:2] == b'\xff\xd8' and data[-2:] == b'\xff\xd9', 'Incomplete JPEG markers')
    spans = []
    pos = 2
    while pos < len(data):
        require(data[pos] == 255, 'Invalid JPEG marker before entropy scan')
        while pos < len(data) and data[pos] == 255:
            pos += 1
        require(pos < len(data), 'Truncated JPEG scan marker')
        marker = data[pos]
        pos += 1
        if marker == 0xD9:
            break
        require(marker != 0, 'Stuffed JPEG byte outside entropy scan')
        if marker in (0x01, 0xD8) or 0xD0 <= marker <= 0xD7:
            continue
        require(pos + 2 <= len(data), 'Truncated JPEG scan segment')
        size = int.from_bytes(data[pos:pos+2], 'big')
        require(size >= 2 and pos + size <= len(data), 'Invalid JPEG scan segment')
        pos += size
        if marker != 0xDA:
            continue
        start = pos
        while pos < len(data):
            if data[pos] != 255:
                pos += 1
                continue
            marker_start = pos
            while pos < len(data) and data[pos] == 255:
                pos += 1
            require(pos < len(data), 'Truncated JPEG entropy marker')
            if data[pos] == 0 or 0xD0 <= data[pos] <= 0xD7:
                pos += 1
                continue
            pos = marker_start
            break
        spans.append({'start': start, 'end': pos, 'bytes': pos - start})
    require(bool(spans), 'JPEG lacks SOS entropy scan')
    longest = {'length': 0, 'offset': None}
    suspect = []
    for span in spans:
        for match in re.finditer(b'\x00+', data[span['start']:span['end']]):
            length = match.end() - match.start()
            offset = span['start'] + match.start()
            run = {'length': length, 'offset': offset, 'offset_mod_64': offset % 64}
            if length > longest['length']:
                longest = run
            if length >= 64:
                suspect.append(run)
    return {'diagnostic': 'entropy_zero_run', 'qualification_only': True,
            'threshold_bytes': 64, 'entropy_spans': spans,
            'longest_zero_run': longest, 'suspect_zero_runs': suspect,
            'passed': not suspect}


def require_test_photo(path, folder):
    require(isinstance(path, str) and path.startswith(folder + '/') and
            all(part not in ('.', '..', '') for part in path[1:].split('/')) and
            path.endswith('.jpg'), 'Refusing photo outside the exact test folder')


def decode_jpeg_file(image_python, path, expected):
    # Run the full decoder independently of the USB Python environment. Pillow
    # verify() alone does not decompress JPEG scan data, so reopen and load too.
    code = """import json, sys, warnings
from PIL import Image, ImageFile, ImageStat
warnings.simplefilter('error')
ImageFile.LOAD_TRUNCATED_IMAGES = False
with Image.open(sys.argv[1]) as image:
    assert image.format == 'JPEG', 'not JPEG'
    image.verify()
with Image.open(sys.argv[1]) as image:
    image.load()
    rgb = image.convert('RGB')
    stats = ImageStat.Stat(rgb)
    print(json.dumps({'width': image.width, 'height': image.height,
                      'decoded': True, 'rgb_mean': stats.mean,
                      'rgb_extrema': stats.extrema}))
"""
    process = subprocess.run([str(image_python), '-c', code, str(path)],
                             capture_output=True, text=True, timeout=30)
    require(process.returncode == 0 and not process.stderr.strip(), 'Full JPEG decoding failed: ' + path.name)
    result = json.loads(process.stdout)
    require((result['width'], result['height']) == tuple(expected), 'Decoded JPEG geometry differs')
    return result


class PhotoChunkTransportError(ValueError):
    """A completed USB command returned an unusable chunk; safe to reread."""


def parse_photo_chunk(response, path, offset, total):
    values = [v for v in extract_json_objects(response) if v.get('success') is True and 'data' in v]
    if len(values) != 1:
        raise PhotoChunkTransportError('missing or ambiguous file chunk')
    item = values[0]
    require(item.get('path') == path and item.get('offset') == offset and item.get('enc') == 'b64',
            'File read path/offset/encoding mismatch')
    require(type(item.get('size')) is int and 0 < item['size'] <= 128*1024, 'Invalid JPEG size')
    require(total is None or item['size'] == total, 'JPEG size changed during read')
    try:
        chunk = base64.b64decode(item['data'], validate=True)
    except (ValueError, TypeError) as error:
        raise PhotoChunkTransportError('invalid base64 file chunk') from error
    if type(item.get('len')) is not int or len(chunk) != item['len'] or not 0 < len(chunk) <= 256:
        raise PhotoChunkTransportError('file chunk length mismatch')
    require(offset + len(chunk) <= item['size'], 'JPEG chunk exceeds total size')
    require(item.get('eof') is (offset + len(chunk) == item['size']), 'File read EOF mismatch')
    return chunk, item['size'], item['eof']


def validate_jpeg_file(validator, path):
    process = subprocess.run([str(validator), str(path)], capture_output=True, text=True, timeout=30)
    require(process.returncode == 0, 'Strict JPEG validation failed: ' + path.name)
    return {'passed': True, 'warning_fatal': True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--credentials', type=Path, required=True)
    parser.add_argument('--p4-port', required=True)
    parser.add_argument('--s3-port', required=True)
    parser.add_argument('--p4-mac', required=True)
    parser.add_argument('--s3-mac', required=True)
    parser.add_argument('--board', choices=('both', 'p4', 's3'), default='both',
                        help='Open only the selected board; interop requires both')
    parser.add_argument('--http', action='store_true')
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--resolution', type=int, choices=range(13),
                        help='Capture only this supported resolution at quality 12')
    parser.add_argument('--captures', type=int, default=1,
                        help='Targeted captures per board (1..20, requires --resolution)')
    parser.add_argument('--image-python', type=Path, default=Path(sys.executable),
                        help='Python with Pillow for full image decoding (defaults to current Python)')
    parser.add_argument('--jpeg-validator', type=Path,
                        help='Optional strict warning-fatal libjpeg validator executable')
    parser.add_argument('--run-root', type=Path, default=HERE/'private/hardware')
    args = parser.parse_args()
    if args.http and args.board != 'both':
        parser.error('--http requires --board both')
    if args.smoke and args.resolution is not None:
        parser.error('--smoke and --resolution are mutually exclusive')
    if not 1 <= args.captures <= 20 or (args.captures != 1 and args.resolution is None):
        parser.error('--captures must be 1..20 and requires --resolution when greater than 1')
    selected = ('p4', 's3') if args.board == 'both' else (args.board,)
    # Fail before opening either USB port if the independent image decoder is missing.
    decoder = subprocess.run([str(args.image_python), '-c', 'from PIL import Image, ImageFile, ImageStat'],
                             capture_output=True, timeout=15)
    require(decoder.returncode == 0, 'Pillow unavailable; pass --image-python with a Pillow-enabled Python')
    credentials = json.loads(args.credentials.read_text())
    run_id = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')+'-'+secrets.token_hex(3)
    run = args.run_root/run_id
    run.mkdir(parents=True, mode=0o700)
    config = {key: {'port': getattr(args, key+'_port'), 'mac': getattr(args, key+'_mac').lower(), 'name':'HW1_'+key.upper()}
              for key in selected}
    original, created, results = {}, {key: [] for key in selected}, {'schema':1, 'run':run_id, 'boards':list(selected), 'checks':[], 'restored':{}, 'transport_retries': {'count': 0, 'file_reads': []}}
    folder = '/camera-qual-'+run_id
    removed = {key: 0 for key in config}
    mutated, loglinked, folders_created = set(), set(), set()
    probe, request, http_started = None, None, False
    def save():
        (run/'results.json').write_text(json.dumps(results, indent=2)+'\n')
        (run/'results.json').chmod(0o600)
    with contextlib.ExitStack() as stack:
        boards = {key: stack.enter_context(RolesConsole(spec['port'], run/(key+'.log'), secrets=private_encodings(credentials), completion='hardwareone'))
                  for key,spec in config.items()}
        time.sleep(9)
        runner = MeshRunner(boards, config, credentials, run, run_id,
                            SimpleNamespace(configure=False, pair='preserve', initiator='p4', rekey=False, reopen=False))
        def command(key, text, timeout=65):
            try:
                return boards[key].command(text, timeout=timeout)
            finally:
                runner.check_health()
        def status(key):
            values = [v for v in extract_json_objects(command(key,'cameraread')) if 'backend' in v and 'resolutions' in v]
            require(len(values)==1, key+': camera status missing')
            return values[0]
        def check(name, fn):
            value = fn()
            runner.check_health()
            results['checks'].append({'name':name, 'passed':True, 'result':value})
            save()
            print(json.dumps({'passed':name}),flush=True)
            return value
        def must(key, text, expected):
            response = command(key,text)
            require(expected in response, key+': command failed: '+text)
            return response
        def read_photo(key,path,chunk_size=256):
            data = bytearray()
            total = None
            while True:
                # A completed command can still lose USB text. Reread exactly
                # this offset up to three times; never advance past bad bytes.
                for attempt in range(4):
                    response = command(key, 'fileread '+command_token(path)+' '+str(len(data))+' '+str(chunk_size)+' b64')
                    try:
                        chunk, size, eof = parse_photo_chunk(response, path, len(data), total)
                        break
                    except PhotoChunkTransportError as error:
                        if attempt == 3:
                            raise PhotoChunkTransportError('File chunk still invalid after three rereads') from error
                        retries = results.setdefault('transport_retries', {'count': 0, 'file_reads': []})
                        retries['count'] += 1
                        retries['file_reads'].append({'board': key, 'offset': len(data),
                                                      'reread': attempt + 1, 'reason': str(error)})
                        save()
                total = size
                data.extend(chunk)
                if eof:
                    require(len(data) == total, 'JPEG total length mismatch')
                    return bytes(data)
        def delete_test_photo(key, path):
            require_test_photo(path, folder)
            must(key, 'filedelete '+command_token(path)+' confirm', 'Deleted file:')
            created[key].remove(path)
            removed[key] += 1
        def qualify_image(data, artifact, dimensions):
            diagnostic = jpeg_entropy_diagnostics(data)
            results.setdefault('image_diagnostics', {})[artifact.name] = diagnostic
            save()  # Retain the corruption signature even if qualification fails.
            diagnostic['strict_validation'] = {'status': 'not_run'}
            try:
                decoded = decode_jpeg_file(args.image_python, artifact, dimensions)
                diagnostic['pillow_decode'] = {'passed': True}
            except Exception:
                diagnostic['pillow_decode'] = {'passed': False}
                save()
                raise
            if args.jpeg_validator is not None:
                try:
                    decoded['strict_validation'] = validate_jpeg_file(args.jpeg_validator, artifact)
                    diagnostic['strict_validation'] = decoded['strict_validation']
                except Exception:
                    diagnostic['strict_validation'] = {'passed': False, 'warning_fatal': True}
                    save()
                    raise
            save()
            require(diagnostic['passed'], 'JPEG entropy zero-run anomaly: '+artifact.name)
            return decoded
        def capture_saved(key,size,label):
            time.sleep(1.05)
            start=time.monotonic()
            response=command(key,'camerasave',timeout=40)
            elapsed=round((time.monotonic()-start)*1000)
            match=re.search(r'Saved: (\S+\.jpg)',response)
            require(match is not None,key+': photo did not save')
            path=match.group(1)
            require_test_photo(path, folder)
            created[key].append(path)
            data=read_photo(key,path)
            dest=run/(key+'-'+label+'.jpg')
            dest.write_bytes(data);dest.chmod(0o600)
            try:
                dims=jpeg_dimensions(data)
                require(dims==(size['width'],size['height']),key+': JPEG actual dimensions differ')
                decoded = qualify_image(data, dest, dims)
            except Exception:
                # Keep the failed run failed. One independent download of the
                # SAME saved file distinguishes repeatable source bytes from
                # a transfer discrepancy without recapturing or masking either.
                reread = results.setdefault('image_rereads', {}).setdefault(dest.name, {
                    'original_sha256': hashlib.sha256(data).hexdigest(),
                    'original_bytes': len(data), 'redownloads': 1, 'reread_chunk_bytes': 128})
                save()
                try:
                    repeated = read_photo(key, path, chunk_size=128)
                    repeated_artifact = dest.with_name(dest.stem+'-reread.jpg')
                    repeated_artifact.write_bytes(repeated);repeated_artifact.chmod(0o600)
                    reread.update({'artifact': repeated_artifact.name,
                                   'sha256': hashlib.sha256(repeated).hexdigest(),
                                   'bytes': len(repeated), 'exact_match': repeated == data})
                    repeated_dims = jpeg_dimensions(repeated)
                    require(repeated_dims == (size['width'], size['height']), 'Reread JPEG geometry differs')
                    reread['decoder'] = qualify_image(repeated, repeated_artifact, repeated_dims)
                    reread['qualification_passed'] = True
                except Exception as error:
                    message = type(error).__name__+': '+str(error)
                    for board in boards.values(): message = board.redact(message)
                    reread['qualification_passed'] = False
                    reread['error'] = message
                save()
                raise
            # Retire each unique test photo immediately; existing retention limits
            # then cannot delete an earlier path before final cleanup reaches it.
            delete_test_photo(key, path)
            return {'width':dims[0],'height':dims[1],'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest(),
                    'capture_command_ms':elapsed,'artifact':dest.name,'decoder':decoded}
        try:
            for key in boards:
                runner.login(key)
                loglinked.add(key)
                must(key,'loglink on','loglink ON')
            for key in boards:
                check(key+'_identity',lambda key=key:runner.inspect(key))
            for key in boards:
                s=status(key)
                folder_reply=command(key,'cameracapturefolder')
                storage_reply=command(key,'camerastoragelocation')
                auto_reply=command(key,'cameraautocapture')
                quality_reply=command(key,'cameraquality')
                original[key]={'camera':s,'folder':re.search(r'cameraCaptureFolder = ([^\r\n]*)',folder_reply).group(1),
                               'storage':int(re.search(r'cameraStorageLocation = (\d+)',storage_reply).group(1)),
                               'autocapture':'= true' in auto_reply,
                               'quality':int(re.search(r'Current: (\d+)',quality_reply).group(1))}
                require(s['requestedFramesize'] in [r['id'] for r in s['resolutions']], 'Original resolution requires manual preservation')
                if args.resolution is not None:
                    require(args.resolution in [r['id'] for r in s['resolutions']], key+': requested qualification resolution unsupported')
                require(not original[key]['autocapture'],'Pre-existing auto capture is active')
                require(bool(original[key]['folder']), 'Empty original capture folder cannot be restored by this CLI')
                command_parts('cameracapturefolder '+original[key]['folder'])
                results.setdefault('initial',{})[key]={'camera':s,'power':find_object(command(key,'power json'),'cpuMhz'),
                                                     'peers':find_object(command(key,'espnowlist json'),'devices'),
                                                     'http':find_object(command(key,'httpstatus json'),'running')}
                save()
            for key in boards:
                s = original[key]['camera']
                must(key, 'mkdir '+command_token(folder), 'Created folder:')
                folders_created.add(key)
                mutated.add(key)
                must(key,'cameracapturefolder '+folder,'cameraCaptureFolder set to')
                must(key,'camerastoragelocation 0','cameraStorageLocation set to 0')
                must(key,'opencamera','Camera started successfully')
                check(key+'_started',lambda key=key:status(key))
                if args.resolution is not None:
                    size=next(r for r in s['resolutions'] if r['id']==args.resolution)
                    must(key,'cameraframesize '+str(size['id']),'Resolution set to')
                    must(key,'cameraquality 12','JPEG quality set to 12')
                    for capture_index in range(1, args.captures + 1):
                        label='size-'+str(size['id'])+'-capture-'+str(capture_index)
                        check(key+'_resolution_'+str(size['id'])+'_capture_'+str(capture_index),
                              lambda key=key,size=size,label=label:capture_saved(key,size,label))
                    continue
                if args.smoke:
                    size=next(r for r in s['resolutions'] if r['id']==s['requestedFramesize'])
                    check(key+'_saved_photo',lambda key=key,size=size:capture_saved(key,size,'smoke'))
                    continue
                for size in s['resolutions']:
                    must(key,'cameraframesize '+str(size['id']),'Resolution set to')
                    must(key,'cameraquality 12','JPEG quality set to 12')
                    check(key+'_resolution_'+str(size['id']),lambda key=key,size=size:capture_saved(key,size,'size-'+str(size['id'])))
                # Small frame endpoint quality values remain safe within the cap.
                must(key,'cameraframesize 10','Resolution set to')
                small=next(r for r in s['resolutions'] if r['id']==10)
                for quality in (0,63):
                    must(key,'cameraquality '+str(quality),'JPEG quality set to '+str(quality))
                    check(key+'_quality_'+str(quality),lambda key=key,q=quality:capture_saved(key,small,'quality-'+str(q)))
                must(key,'cameraquality 12','JPEG quality set to 12')
                before=status(key)
                must(key,'cameratiny','Tiny frame (160x120):')
                after=status(key)
                require((before['framesize'],before['quality'])==(after['framesize'],after['quality']), 'Tiny frame did not restore geometry/quality')
                check(key+'_tiny_restores',lambda: {'framesize':after['framesize'],'quality':after['quality']})
                for _ in range(3):
                    must(key,'closecamera','Camera stopped')
                    require(status(key)['enabled'] is False,'Camera still enabled')
                    must(key,'opencamera','Camera started successfully')
                    must(key,'cameracapture','Captured frame:')
                check(key+'_restart_cycles',lambda:{'cycles':3})
                before=status(key)
                must(key,'cameraframesize 5','unsupported')
                require(status(key)['requestedFramesize']==before['requestedFramesize'],'Unsupported resolution persisted')
                if key=='p4':
                    must(key,'camerabrightness 1','unsupported')
                    require('brightness' not in status(key)['controls'],'P4 advertised unsupported brightness')
                    check('p4_unsupported_control',lambda:{'rejected':True})
            if args.board == 'both' and ((not args.smoke and args.resolution is None) or args.http):
                check('existing_encrypted_pair',runner.pair)
                for sender,receiver in (('p4','s3'),('s3','p4')):
                    check(sender+'_mesh_while_cameras_running',lambda sender=sender,receiver=receiver:runner.text(sender,receiver,401,phase='camera'))
                if args.http:
                    require(not results['initial']['p4']['http']['running'],'Existing HTTP service must be preserved')
                    ssid=credentials.get('ap_ssid','HW1_P4_TEST')
                    http_started = True
                    results['http_runtime_reboot_required'] = True
                    save()
                    ap=find_object(command('p4','probeap '+command_token(ssid)+' '+command_token(credentials['ap_password'])),'ap')
                    require(ap.get('ap') and ap.get('http') and ap.get('channel')==6,'P4 AP fixture failed')
                    probe=HttpS3Probe(boards['s3'],credentials)
                    check('s3_http_join',probe.join)
                    def request(path,fields=None):
                        method='POST' if fields is not None else 'GET'
                        arg='request '+method+' '+path
                        if fields is not None:arg+=' '+base64.b64encode(urllib.parse.urlencode(fields).encode()).decode()
                        try:
                            reply=validate_reply(probe.rpc(arg))
                        finally:
                            runner.check_health()
                        results.setdefault('http_requests',[]).append({'path':path,**reply.metadata})
                        return reply
                    probe.rpc('clear');runner.check_health()
                    require_denied(request('/api/sensors/camera/frame'))
                    login=request('/login',{k:credentials[k] for k in ('username','password')})
                    require(login.status==303 and login.metadata.get('cookiePresent'),'HTTP login failed')
                    page=request('/sensors')
                    require(page.status==200 and page.metadata.get('htmlOpen') and page.metadata.get('htmlClose'),'Sensors page incomplete')
                    response=request('/api/sensors/camera/status')
                    require(response.status==200 and json.loads(response.text()).get('hardwareJpeg') is True,'P4 HTTP camera status failed')
                    # Low-quality tiny image fits the existing bridge's 8 KiB body window.
                    must('p4','cameraframesize 7','Resolution set to')
                    must('p4','cameraquality 55','JPEG quality set to')
                    response=request('/api/sensors/camera/frame')
                    require(response.status==200 and response.body is not None,'HTTP JPEG body unavailable')
                    require(jpeg_dimensions(response.body)==(160,120),'HTTP JPEG geometry wrong')
                    artifact = run/'p4-http.jpg'
                    artifact.write_bytes(response.body);artifact.chmod(0o600)
                    decoded = qualify_image(response.body, artifact, (160, 120))
                    check('p4_web_camera',lambda:{'frame':response.metadata,'dimensions':[160,120],'decoder':decoded})
                    check('s3_capture_during_wifi',lambda: {'reply':boards['s3'].redact(must('s3','cameracapture','Captured frame:'))})
                    request('/logout');require_denied(request('/api/sensors/camera/frame'));probe.rpc('clear');runner.check_health()
                    results['http_runtime_reboot_required']=True
            results['status']='passed'
        except BaseException as error:
            results['status']='failed'
            message=str(error)
            for board in boards.values():message=board.redact(message)
            results['error']=type(error).__name__+': '+message
            # Complete cleanup and return failure below; never print a raw
            # exception/traceback containing private serial command data.
        finally:
            def cleanup(record, label, action):
                try:
                    value = action()
                    record.setdefault('steps', {})[label] = {'ok': True}
                    return value
                except BaseException as error:
                    message = type(error).__name__ + ': ' + str(error)
                    for board in boards.values(): message = board.redact(message)
                    record.setdefault('steps', {})[label] = {'ok': False, 'error': message}
                    record.setdefault('errors', []).append(label + ': ' + message)
                    record['error'] = '; '.join(record['errors'])
                    return None
            if http_started:
                network = results.setdefault('http_cleanup', {})
                # Each cleanup is independent. A failed logout must not prevent
                # clearing S3's cookie or stopping the temporary P4 HTTP server.
                if request is not None:
                    cleanup(network, 'logout', lambda: request('/logout'))
                if probe is not None:
                    def clear_cookie():
                        reply = probe.rpc('clear')
                        runner.check_health()
                        require(reply.get('cookiePresent') is False, 'HTTP cookie was not cleared')
                    cleanup(network, 'clear_cookie', clear_cookie)
                def close_http():
                    command('p4', 'closehttp')
                    state = find_object(command('p4', 'httpstatus json'), 'running')
                    require(state.get('running') is False, 'Temporary HTTP server still running')
                cleanup(network, 'close_http', close_http)
            for key in boards:
                record = results['restored'].setdefault(key, {})
                if key in mutated:
                    old = original[key]
                    # Restore capture destinations first, even if a camera power
                    # transition or a later setting cannot be restored.
                    cleanup(record, 'folder', lambda key=key,old=old:
                            must(key, 'cameracapturefolder '+old['folder'], 'cameraCaptureFolder set to'))
                    cleanup(record, 'storage', lambda key=key,old=old:
                            must(key, 'camerastoragelocation '+str(old['storage']), 'cameraStorageLocation set to'))
                    cleanup(record, 'close_camera', lambda key=key: must(key,'closecamera','Camera stopped'))
                    cleanup(record, 'resolution', lambda key=key,old=old:
                            must(key,'cameraframesize '+str(old['camera']['requestedFramesize']),'Resolution set to'))
                    cleanup(record, 'quality', lambda key=key,old=old:
                            must(key,'cameraquality '+str(old['quality']),'JPEG quality set to'))
                    if old['camera']['enabled']:
                        cleanup(record, 'open_camera', lambda key=key: must(key,'opencamera','Camera started successfully'))
                    state = cleanup(record, 'camera_status', lambda key=key: status(key))
                    if state is not None:
                        record['camera'] = state
                        cleanup(record, 'camera_state_matches', lambda old=old,state=state:
                                require(state['requestedFramesize'] == old['camera']['requestedFramesize'] and
                                        state['enabled'] == old['camera']['enabled'], 'Camera state differs after restore'))
                for path in tuple(created[key]):
                    cleanup(record, 'delete_'+path.rsplit('/',1)[-1],
                            lambda key=key,path=path: delete_test_photo(key,path))
                if key in folders_created:
                    cleanup(record, 'remove_folder', lambda key=key:
                            must(key,'rmdir '+command_token(folder),'Removed folder:'))
                if key in loglinked:
                    cleanup(record, 'loglink_off', lambda key=key: command(key,'loglink off'))
                record['settings'] = 'error' not in record
                record['test_photos_removed'] = removed[key]
                record['test_photos_remaining'] = len(created[key])
            if 'error' in results.get('http_cleanup', {}):
                results['status'] = 'failed'
            if any('error' in entry for entry in results['restored'].values()):
                results['status'] = 'failed'
            save()
    print(json.dumps({'status':results['status'],'checks':len(results['checks']),'run':str(run),'restored':{k:'error' not in v for k,v in results['restored'].items()},'error':results.get('error')}),flush=True)
    return 0 if results['status']=='passed' and all('error' not in v for v in results['restored'].values()) else 1

if __name__=='__main__':
    try:
        exit_code = main()
    except Exception as error:
        # Setup errors happen before the redacting consoles exist. Do not
        # render their raw text (which can contain a command or credential).
        print(json.dumps({'status':'failed', 'error':type(error).__name__,
                          'detail':'Qualification setup failed; check private inputs and the Pillow runtime.'}),file=sys.stderr)
        exit_code = 1
    raise SystemExit(exit_code)
