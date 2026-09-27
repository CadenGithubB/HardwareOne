#!/usr/bin/env python3
"""Opt-in real HardwareOne HTTP checks. No network actions on import.

Uses private test credentials; never logs passwords, cookies or response headers.
--duration repeats authenticated reads for concurrent BLE/mesh testing.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import http.cookiejar
import json
import os
from pathlib import Path
import secrets
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = Path(__file__).resolve().parent


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class HttpProbe:
    def __init__(self, base: str, credentials: dict):
        parsed = urllib.parse.urlsplit(base)
        if parsed.scheme != 'http' or not parsed.hostname or parsed.username or parsed.password:
            raise ValueError('Expected a plain HTTP test-device URL without credentials')
        self.base = base.rstrip('/')
        self.credentials = credentials
        self.cookies = http.cookiejar.CookieJar()
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}),
            urllib.request.HTTPCookieProcessor(self.cookies), NoRedirect())
        self.evidence = []
        self.last_command_started = 0.0

    def request(self, path: str, fields: dict | None = None):
        data = urllib.parse.urlencode(fields).encode() if fields is not None else None
        request = urllib.request.Request(self.base + path, data=data)
        if data is not None:
            request.add_header('Content-Type', 'application/x-www-form-urlencoded')
        started = time.monotonic()
        try:
            response = self.opener.open(request, timeout=20)
        except urllib.error.HTTPError as exc:
            response = exc
        with response:
            raw = response.read(2_000_001)
            if len(raw) > 2_000_000:
                raise AssertionError('HTTP response exceeded the test bound')
            self.evidence.append({'path': path, 'method': 'POST' if data else 'GET',
                                  'status': response.status, 'bytes': len(raw),
                                  'sha256': hashlib.sha256(raw).hexdigest(),
                                  'durationMs': round((time.monotonic()-started)*1000, 2)})
            return response.status, raw.decode('utf-8'), response.headers.get('Location', '')

    def command(self, command: str):
        time.sleep(max(0.0, 0.075 - (time.monotonic() - self.last_command_started)))
        self.last_command_started = time.monotonic()
        status, body, _ = self.request('/api/cli', {'cmd': command, 'capture': '1'})
        if status != 200 or 'Unknown command' in body or body.startswith(('ERROR:', 'Error:')):
            raise AssertionError('Authenticated HTTP command failed: ' + command)
        return body

    def verify(self, duration: float):
        status, body, _ = self.request('/login')
        assert status == 200 and 'password' in body and 'username' in body, 'Login form unavailable'
        status, body, location = self.request('/api/system')
        assert status in (401, 403) or (status in (302, 303) and location.startswith('/login')), \
            'Protected API did not reject unauthenticated request'
        status, _, location = self.request('/login', {k:self.credentials[k] for k in ('username','password')})
        assert status == 303 and location == '/dashboard' and list(self.cookies), 'Cookie login failed'
        for path in ('/dashboard', '/settings', '/bluetooth', '/espnow', '/cli'):
            status, body, _ = self.request(path)
            assert status == 200 and '<html' in body.lower() and '</html>' in body.lower(), 'Incomplete page: '+path
        for path in ('/api/system', '/api/buildconfig'):
            status, body, _ = self.request(path)
            document = json.loads(body)
            assert status == 200 and isinstance(document, dict) and 'error' not in document, 'Invalid API: '+path
        identity = self.command('whoami')
        assert self.credentials['username'] in identity and 'admin' in identity, 'Wrong HTTP command identity'
        for command in ('status json', 'blestatus json', 'espnowstatus json'):
            body = self.command(command)
            # Prefix text is allowed by the CLI; require an actual complete JSON object.
            begin = body.find('{')
            document, _ = json.JSONDecoder().raw_decode(body[begin:])
            assert begin >= 0 and isinstance(document, dict), 'Missing command JSON'
        deadline = time.monotonic() + duration
        cycles = 0
        while time.monotonic() < deadline:
            for path in ('/api/system', '/bluetooth'):
                status, body, _ = self.request(path)
                assert status == 200 and body, 'Concurrent HTTP read failed'
            cycles += 1
            time.sleep(0.4)
        self.request('/logout')
        status, _, location = self.request('/api/system')
        assert status in (401,403) or (status in (302,303) and location.startswith('/login')), 'Logout failed'
        return {'cookieLogin': True, 'protectedApi': True, 'logout': True, 'loadCycles': cycles}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://192.168.4.1')
    parser.add_argument('--credentials', type=Path, default=HERE/'private/credentials.json')
    parser.add_argument('--duration', type=float, default=0)
    args = parser.parse_args(argv)
    if not 0 <= args.duration <= 600:
        parser.error('duration must be between 0 and 600 seconds')
    credentials = json.loads(args.credentials.read_text())
    private_values = [v for k,v in credentials.items() if k != 'mesh_label' and isinstance(v,str)]
    def safe(value):
        for secret in sorted(private_values, key=len, reverse=True):
            value = value.replace(secret, '[REDACTED]')
        return value
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    run = HERE/'private/http-runs'/(stamp+'-'+secrets.token_hex(4))
    run.mkdir(parents=True, mode=0o700)
    result = {'started':stamp, 'url':args.url, 'durationSec':args.duration, 'status':'running'}
    probe = HttpProbe(args.url, credentials)
    try:
        result['verified'] = probe.verify(args.duration)
        result['status'] = 'passed'
    except Exception as exc:
        result.update(status='failed', error=safe(type(exc).__name__+': '+str(exc)))
    result['requests'] = probe.evidence
    path = run/'results.json'
    fd = os.open(path, os.O_WRONLY|os.O_CREAT|os.O_EXCL, 0o600)
    with os.fdopen(fd,'w') as stream:
        stream.write(safe(json.dumps(result,indent=2))+'\n')
    print(result['status'].upper(), len(probe.evidence), 'HTTP requests;', path, flush=True)
    if result['status'] != 'passed': print(result.get('error',''), flush=True)
    return 0 if result['status']=='passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
