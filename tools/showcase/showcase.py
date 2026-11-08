#!/usr/bin/env python3
"""HardwareOne Showcase: a live, narrated view of what a device is doing.

Runs on a laptop. Nothing on the device changes: it reads the device's
existing web API (HTTP Basic Auth, stateless, so your own browser login on the
device is never bumped) or the USB serial console, and serves a presentation
page on localhost that turns the device's state into a story as it changes.

Python 3 standard library only. pyserial is optional (serial transport).

    python3 tools/showcase/showcase.py --host 192.168.1.42 --user admin
    python3 tools/showcase/showcase.py --serial /dev/cu.usbmodem14201 --user admin
    python3 tools/showcase/showcase.py --mock          # rehearse with fake data

Then open the printed URL. Press T on the page for the guided tour.
"""
import argparse
import base64
import getpass
import json
import os
import random
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
SERIES_LEN = 900          # points per sparkline (at 2 s, 30 minutes)
TRAIL_LEN = 150           # task activity samples kept (at 2 s, 5 minutes)
MOMENTS_LEN = 120
CONSOLE_LEN = 80
EVENTS_LEN = 48           # the device ring holds 48


# --------------------------------------------------------------------------
# Transports
# --------------------------------------------------------------------------

class TransportError(Exception):
    pass


class HttpTransport:
    """Talks to the device web server with HTTP Basic Auth."""
    name = 'http'

    def __init__(self, base, user, password, timeout=8.0):
        self.base = base.rstrip('/')
        self.timeout = timeout
        token = base64.b64encode(f'{user}:{password}'.encode()).decode()
        self._auth = 'Basic ' + token
        self._last_cmd = 0.0
        self.target = self.base

    def _open(self, path, data=None, headers=None, raw=False):
        req = urllib.request.Request(self.base + path, data=data, method='POST' if data is not None else 'GET')
        req.add_header('Authorization', self._auth)
        req.add_header('Accept', 'application/json, text/plain')
        for k, v in (headers or {}).items():
            req.add_header(k, v)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                body = resp.read()
                return resp.status, body if raw else body.decode('utf-8', 'replace')
        except urllib.error.HTTPError as e:
            body = e.read()
            return e.code, body if raw else body.decode('utf-8', 'replace')
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            raise TransportError(str(e.reason if hasattr(e, 'reason') else e))

    def get_json(self, path):
        status, body = self._open(path)
        if status == 401:
            raise TransportError('device rejected the credentials (401)')
        if status == 403:
            return {'_forbidden': True, '_status': 403}
        if status != 200:
            return {'_status': status, '_body': body[:200]}
        try:
            return json.loads(body)
        except ValueError:
            return {'_status': status, '_body': body[:200]}

    def get_text(self, path):
        status, body = self._open(path)
        if status == 401:
            raise TransportError('device rejected the credentials (401)')
        return status, body

    def get_bytes(self, path):
        return self._open(path, raw=True)

    def command(self, cmd, capture=False):
        """Run one CLI command. Returns the raw text result."""
        gap = 0.08 - (time.monotonic() - self._last_cmd)
        if gap > 0:
            time.sleep(gap)
        fields = {'cmd': cmd}
        if capture:
            fields['capture'] = '1'
        data = urllib.parse.urlencode(fields).encode()
        for attempt in range(3):
            status, body = self._open('/api/cli', data=data,
                                      headers={'Content-Type': 'application/x-www-form-urlencoded'})
            self._last_cmd = time.monotonic()
            if status == 429:
                time.sleep(0.15)
                continue
            if status == 401:
                raise TransportError('device rejected the credentials (401)')
            return status, body
        return status, body

    def command_json(self, cmd):
        status, body = self.command(cmd)
        return parse_first_json(body, status)

    def batch_json(self, cmds):
        """Run several JSON commands in one request. Returns a list of parsed results."""
        data = json.dumps({'commands': cmds}).encode()
        status, body = self._open('/api/cli/batch', data=data, headers={'Content-Type': 'application/json'})
        if status == 401:
            raise TransportError('device rejected the credentials (401)')
        try:
            doc = json.loads(body)
            results = doc.get('results', [])
        except ValueError:
            results = []
        out = []
        for i, _ in enumerate(cmds):
            out.append(parse_first_json(results[i], 200) if i < len(results) else None)
        return out

    def has_endpoints(self):
        return True


class SerialTransport:
    """Talks to the USB console. Needs pyserial (the ESP-IDF venv has it).

    Opens without resetting the chip (DTR raised before RTS, never RTS alone).
    The device endpoints are emulated with the matching `<cmd> json` commands.
    """
    name = 'serial'
    PROMPT = b'$ '

    def __init__(self, port, user, password, baud=115200):
        try:
            import serial  # noqa: F401
        except ImportError:
            raise TransportError('pyserial is not installed; use the ESP-IDF python '
                                 '(~/.espressif/python_env/*/bin/python3) or pip install pyserial')
        self.port, self.baud = port, baud
        self.lock = threading.Lock()
        self.target = port
        self.console = deque(maxlen=CONSOLE_LEN)
        self.user, self.password = user, password
        self.ser = None
        self.reopen()  # raises TransportError while the port is absent

    def _open(self):
        """Open without resetting the chip (DTR high then low with RTS low)."""
        import serial
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = self.port, self.baud, 0.2
        s.dtr, s.rts = True, False
        s.open()
        time.sleep(0.05)
        s.dtr = False
        self.ser = s

    def close(self):
        """Sign the serial session out before letting go of the port. The
        device keeps a serial login alive until logout or its idle timeout
        (default 60 min), so a tool that just exits would leave an admin
        console open for whoever plugs in next."""
        try:
            with self.lock:
                self.ser.write(b'logout\n')
                self._read_reply(2.0, done=lambda ln: 'logged out' in ln.lower() or 'signed out' in ln.lower() or ln.startswith('OK'))
        except Exception:
            pass
        try:
            self.ser.close()
        except Exception:
            pass

    def reopen(self):
        """After the USB device vanished (Mac sleep, re-enumeration, a reboot):
        drop the dead handle, open the port again and sign in. Raises
        TransportError while the port is still absent."""
        import serial
        try:
            if self.ser:
                try: self.ser.close()
                except Exception: pass
            self.ser = None
            self._open()
            self._drain(0.3)
            self._login()
        except (serial.SerialException, OSError) as e:
            raise TransportError(f'serial port not back yet: {e}')

    def _drain(self, seconds):
        end = time.monotonic() + seconds
        buf = b''
        while time.monotonic() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
        self._absorb_console(buf)
        return buf

    def _absorb_console(self, data):
        for line in data.decode('utf-8', 'replace').splitlines():
            line = line.strip()
            if line and line != '$':
                self.console.append(line)

    LOG_LINE = re.compile(r'^[IWEDV] \(\d+\) ')
    BCAST_LINE = re.compile(r'^\[\d+\] ')

    def _login(self):
        # A device that is still booting answers nothing; that is not a rejection.
        # Three spaced tries stay well under the five failures that lock serial login.
        for attempt in range(3):
            self.ser.write(f'login {self.user} {self.password}\n'.encode())
            out = self._read_reply(4.0, done=lambda ln: 'Login successful' in ln or 'Authentication failed' in ln
                                   or 'locked out' in ln.lower() or 'Session changed' in ln)
            text = ' '.join(out)
            if 'locked out' in text.lower():
                raise TransportError('serial login locked out; wait 30 s before retrying')
            if 'Login successful' in text:
                return
            if 'Authentication failed' in text:
                raise TransportError('serial login failed: the device rejected the username/password')
            if attempt < 2:
                print('device not answering the login yet (booting?); retrying in 8 s', file=sys.stderr)
                time.sleep(8.0)
        raise TransportError('serial login got no answer from the device')

    def _read_reply(self, timeout, done=None, quiet=0.35):
        """Collect console lines until `done` matches, or the console goes quiet
        after at least one non-log line, or `timeout`. The prompt "$ " is
        printed before the command runs, so it is not an end marker."""
        end = time.monotonic() + timeout
        buf, lines, seen_result, last_rx = b'', [], False, None
        while time.monotonic() < end:
            chunk = self.ser.read(4096)
            if chunk:
                buf += chunk
                last_rx = time.monotonic()
                while b'\n' in buf:
                    raw, buf = buf.split(b'\n', 1)
                    ln = raw.decode('utf-8', 'replace').strip()
                    while ln.startswith('$ '):
                        ln = ln[2:].lstrip()
                    if not ln or ln == '$':
                        continue
                    if self.LOG_LINE.match(ln):
                        if 'STT: TL ' not in ln or re.search(r'STT: TL (capture_start|warm|boundary|fe_end|spk) ', ln):
                            self._console_push(ln)
                        continue
                    lines.append(ln)
                    seen_result = True
                    if done and done(ln):
                        return lines
            elif seen_result and last_rx and time.monotonic() - last_rx > quiet:
                break
        return lines

    REPEAT = re.compile(r'\s+×(\d+)$')

    def _console_push(self, ln):
        """Append a log line; a repeat of the previous line (timestamp aside)
        becomes a count on that line instead of another row, so a fault that
        logs every 30 ms cannot push everything else out of the mirror."""
        body = self.LOG_LINE.sub('', ln)
        if self.console:
            last = self.console[-1]
            m = self.REPEAT.search(last)
            last_body = self.LOG_LINE.sub('', self.REPEAT.sub('', last))
            if last_body == body:
                n = int(m.group(1)) + 1 if m else 2
                self.console[-1] = f'{ln} ×{n}'
                return
        self.console.append(ln)

    def command(self, cmd, capture=False):
        import serial
        if self.ser is None:  # a reopen() failed and left no handle; the poller retries later
            raise TransportError('serial port not open')
        try:
            return self._command(cmd, capture)
        except (serial.SerialException, OSError) as e:
            raise TransportError(f'serial port lost: {e}')

    def _command(self, cmd, capture=False):
        want_json = cmd.endswith(' json') or cmd in ('espnowlist', 'espnowmeshstatus')
        # A long JSON reply reaches the console split across several lines (the
        # break can land mid-token), so "done" means the braces balance, not the
        # first line that starts with '{'.
        acc = []
        def json_done(ln):
            if not acc and not ln.startswith('{'):
                return False
            acc.append(ln)
            try:
                json.JSONDecoder().raw_decode(''.join(acc))
                return True
            except ValueError:
                return False
        with self.lock:
            self.ser.write((cmd + '\n').encode())
            lines = self._read_reply(8.0, done=json_done if want_json else
                                     (lambda ln: ln.startswith('Hint:') or ln.startswith('Unknown command')))
        if any('Authentication required' in ln for ln in lines):
            self._login()
            return self.command(cmd, capture)
        result = [ln for ln in lines if not self.BCAST_LINE.match(ln)]
        broadcast = [self.BCAST_LINE.sub('', ln).strip() for ln in lines if self.BCAST_LINE.match(ln)]
        own = f'[CMD] {self.user}@serial:'
        for ln in broadcast:
            if ln.startswith(own):
                continue  # the showcase's own polling; the port is ours while it runs
            if not re.match(r'^\[\d+\] ', ln) and not ln.startswith('OK: ') and not ln.startswith('Hint'):
                self.console.append(ln)
        if want_json and acc:
            # splice the JSON fragments back together without the injected newlines
            head = [ln for ln in result if ln not in acc]
            result = head + [''.join(acc)]
        text = '\n'.join(broadcast + result if capture else result)
        status = 400 if text.startswith('Error') or text.startswith('Unknown command') else 200
        if text.startswith('Error: Admin access required'):
            status = 403
        return status, text

    def command_json(self, cmd):
        status, body = self.command(cmd)
        return parse_first_json(body, status)

    def batch_json(self, cmds):
        return [self.command_json(c) for c in cmds]

    # Endpoint emulation ----------------------------------------------------
    _endpoint_cmd = {
        '/api/system': 'status json',
        '/api/power/status': 'power json',
        '/api/battery/status': 'batterystatus json',
        '/api/ble/status': 'blestatus json',
        '/api/sensors/status': 'sensors json brief',
    }

    G2_TEXT = re.compile(r'state=(\S+) L=(up|down) R=(up|down) mtu=L(\d+)/R(\d+) batt=L(-?\d+)/R(-?\d+)')

    def get_json(self, path):
        cmd = self._endpoint_cmd.get(path.split('?')[0])
        if not cmd:
            return {'_status': 404}
        doc = self.command_json(cmd)
        if path == '/api/ble/status' and isinstance(doc, dict):
            # blestatus json has no ring block (the web endpoint does), so ask the ring module
            ring = self.command_json('ring status json')
            if isinstance(ring, dict) and 'connected' in ring:
                doc['ring'] = ring
            status, text = self.command('g2status', capture=True)
            m = self.G2_TEXT.search(text or '')
            if m:
                st, l, r, lm, rm, lb, rb = m.groups()
                doc['g2'] = {'connection': {'state': st, 'left': {'connected': l == 'up', 'mtu': int(lm), 'battery': int(lb)},
                                            'right': {'connected': r == 'up', 'mtu': int(rm), 'battery': int(rb)}}}
        return doc

    def get_text(self, path):
        return 404, ''  # the serial console ring is merged by the poller directly

    def get_bytes(self, path):
        return 404, b''

    def has_endpoints(self):
        return False


class MockTransport:
    """Fake device for rehearsing the page without hardware."""
    name = 'mock'

    def __init__(self):
        self.t0 = time.time()
        self.target = 'mock device'
        self.rng = random.Random(7)
        self.mhz = 80
        self.heap = 150_000
        self.rssi = -55
        self.g2 = False
        self.seq = 12
        self.events = deque(maxlen=48)
        self.console = deque(maxlen=60)
        self.lastflip = 0
        self.sensors = {'thermal': True, 'tof': False, 'imu': True}
        self.battery = 83.0
        self.charging = True
        for k in ('boot_started', 'wifi_connected', 'time_synced', 'boot_finished'):
            self._post(k, 'system', '', '')

    def _post(self, kind, source, subject, detail, who=''):
        self.seq += 1
        self.events.appendleft({'seq': self.seq, 't': time.time(), 'kind': kind, 'subject': subject,
                                'detail': detail, 'source': source, 'who': who})

    def _tick(self):
        r = self.rng
        now = time.time()
        self.heap = max(60_000, min(190_000, self.heap + r.randint(-4000, 4000)))
        self.rssi = max(-85, min(-35, self.rssi + r.randint(-3, 3)))
        if now - self.lastflip > 9:
            self.lastflip = now
            roll = r.random()
            if roll < 0.3:
                self.mhz = 240 if self.mhz == 80 else 80
                self._post('power_mode_changed', 'system', 'Adaptive', f'{self.mhz} MHz')
            elif roll < 0.5:
                self.g2 = not self.g2
                self._post('g2_connected' if self.g2 else 'g2_disconnected', 'g2', 'Even G2', '')
            elif roll < 0.7:
                k = r.choice(list(self.sensors))
                self.sensors[k] = not self.sensors[k]
                self._post('sensor_started' if self.sensors[k] else 'sensor_stopped', 'cli', k, '', 'admin')
                self.console.append(f'[{int((now - self.t0) * 1000)}] {k}start -> OK')
            elif roll < 0.85:
                self.charging = not self.charging
                self._post('usb_on' if self.charging else 'usb_off', 'system', '', '')
            else:
                self._post('login_ok', 'web', 'admin', '192.168.1.10')
                self.console.append(f'[{int((now - self.t0) * 1000)}] [login] Login successful for user: admin')
        self.battery += 0.02 if self.charging else -0.03

    def get_json(self, path):
        self._tick()
        up = int(time.time() - self.t0) + 3600 * 5
        if path == '/api/system':
            return {'schema': 1, 'fw': '0.99.99', 'board': 'FeatherS3 (mock)', 'reset_reason': 'POWERON',
                    'crash_count': 0, 'system_time': time.strftime('%Y-%m-%dT%H:%M:%S'),
                    'uptime_hms': f'{up // 3600}h {(up % 3600) // 60}m {up % 60}s',
                    'net': {'wifiConnected': True, 'radioOn': True, 'ssid': 'LabNet', 'ip': '192.168.1.42',
                            'rssi': self.rssi, 'channel': 6, 'mac': 'AA:BB:CC:DD:EE:FF'},
                    'mem': {'heap_free_kb': self.heap // 1024, 'heap_total_kb': 230, 'psram_total_kb': 8192,
                            'psram_free_kb': 6100 + self.rng.randint(-50, 50)},
                    'storage': {'total_kb': 3072, 'used_kb': 1310, 'free_kb': 1762},
                    'connectivity': {
                        'espnow': {'enabled': True, 'running': True, 'mesh': True, 'deviceName': 'bench', 'encrypted': True},
                        'mqtt': {'enabled': False, 'connected': False, 'host': ''},
                        'bluetooth': {'running': True, 'state': 'central', 'mode': 'glasses',
                                      'g2Connected': self.g2, 'phoneConnected': False, 'phoneState': 'idle'},
                        'webserver': {'running': True, 'https': False, 'port': 80, 'sessions': 1, 'maxSessions': 2},
                        'i2c': {'compiled': True, 'enabled': True, 'devices': 4, 'activeDevices': 3, 'sdaPin': 8, 'sclPin': 9,
                                'deviceList': [{'address': '0x33', 'name': 'MLX90640 thermal'}, {'address': '0x29', 'name': 'VL53L0X ToF'},
                                               {'address': '0x6A', 'name': 'LSM6DS3 IMU'}, {'address': '0x3C', 'name': 'SSD1306 OLED'}]}}}
        if path == '/api/power/status':
            return {'schema': 2, 'mode': 1, 'modeName': 'Adaptive', 'cpuMhz': self.mhz, 'targetMhz': self.mhz,
                    'activeMhz': 240, 'idleMhz': 80, 'maxMhz': 240, 'boost': self.mhz == 240,
                    'autoMode': True, 'idleMsAgo': self.rng.randint(0, 9000), 'sleepAllowed': False,
                    'displayBrightness': 128}
        if path == '/api/battery/status':
            return {'schema': 1, 'present': True, 'voltage': 3.9, 'percentage': round(self.battery, 1),
                    'charging': self.charging, 'usbPresent': self.charging, 'status': 'charging' if self.charging else 'discharging',
                    'percentageValid': True}
        if path == '/api/ble/status':
            return {'schema': 2, 'running': True, 'state': 'central', 'mode': 'glasses',
                    'g2': {'autoReconnect': True, 'connection': {'running': True, 'state': 'connected' if self.g2 else 'scanning',
                           'left': {'connected': self.g2, 'battery': 71, 'mtu': 247}, 'right': {'connected': self.g2, 'battery': 68, 'mtu': 247}},
                           'controls': {'headUp': self.g2, 'notifications': True}, 'firmware': {'left': '1.6.3', 'right': '1.6.3'}},
                    'ring': {'connected': False},
                    'phoneLink': {'state': 'scanning', 'enrolled': True, 'connected': False}}
        if path == '/api/sensors/status':
            return {'seq': self.seq, 'thermalRunning': self.sensors['thermal'], 'tofRunning': self.sensors['tof'],
                    'imuRunning': self.sensors['imu'], 'thermalCompiled': True, 'tofCompiled': True, 'imuCompiled': True,
                    'gpsCompiled': False, 'cameraConnected': False}
        return {'_status': 404}

    def get_text(self, path):
        return 200, '\n'.join(self.console)

    def get_bytes(self, path):
        return 404, b''

    def command(self, cmd, capture=False):
        if cmd == 'events':
            lines = [f'OK: {self.seq} event(s) posted since boot (ring holds last 48)']
            for e in self.events:
                ago = int(time.time() - e['t'])
                sub = e['subject'] + (f" ({e['detail']})" if e['detail'] else '')
                who = f"  by {e['source']}" + (f":{e['who']}" if e['who'] else '')
                lines.append(f"  [{e['seq']}] {ago}s ago  {e['kind']:<17} {sub}{who}")
            return 200, '\n'.join(lines)
        return 200, 'OK'

    def command_json(self, cmd):
        return self.batch_json([cmd])[0]

    def batch_json(self, cmds):
        out = []
        for c in cmds:
            if c.startswith('memreport'):
                out.append({'schema': 1, 'dram': {'total': 235520, 'used': 235520 - self.heap, 'free': self.heap,
                                                   'minFree': 58000, 'peakUsed': 177000},
                            'psram': {'available': True, 'total': 8388608, 'used': 2_100_000, 'free': 6_288_608},
                            'heapCaps': {'internalFree': self.heap, 'internalLargest': 40000}})
            elif c.startswith('perftop'):
                busy = 25 if self.mhz == 240 else 6
                out.append({'schema': 1, 'loop': {'valid': True, 'lapsPerSec': 410, 'avgMs': 2, 'maxMs': 31, 'stalls5s': 0, 'totalStalls': 3},
                            'tasks': [{'name': 'IDLE0', 'cpuPct': 95 - busy, 'idle': True}, {'name': 'IDLE1', 'cpuPct': 88 - busy, 'idle': True},
                                      {'name': 'loopTask', 'cpuPct': busy // 2 + 1, 'idle': False}, {'name': 'httpd', 'cpuPct': 3, 'idle': False},
                                      {'name': 'sensor_thermal', 'cpuPct': busy // 3 + 1, 'idle': False}, {'name': 'btController', 'cpuPct': 2, 'idle': False}],
                            'cpuValid': True})
            elif c.startswith('taskstats'):
                names = ['loopTask', 'httpd', 'sensor_thermal', 'sensor_imu', 'btController', 'BTC_TASK', 'wifi', 'tiT', 'esp_timer', 'IDLE0', 'IDLE1', 'ipc0', 'ipc1', 'cmd_exec']
                out.append({'schema': 1, 'total': len(names), 'tasks': [
                    {'name': n, 'state': 'BLOCK' if i else 'RUN', 'prio': 1 + i % 5, 'stackFree': 900 + i * 110, 'stackSize': 4096}
                    for i, n in enumerate(names)]})
            elif c.startswith('espnowmeshstatus'):
                out.append({'schema': 1, 'peers': [{'mac': '11:22:33:44:55:66', 'name': 'qtpy-lab', 'alive': True, 'linkRssi': -61, 'heartbeatCount': 410}],
                            'totalPeers': 1, 'unpaired': [], 'totalUnpaired': 0})
            else:
                out.append(None)
        return out

    def has_endpoints(self):
        return True


def parse_first_json(text, status=200):
    """Pull the first JSON object out of a command result blob."""
    if text is None:
        return None
    if status == 403 or text.startswith('Error: Admin access required'):
        return {'_forbidden': True}
    if text.startswith('Unknown command'):
        return {'_unavailable': True}
    start = text.find('{')
    if start < 0:
        return {'_text': text[:200], '_status': status}
    try:
        obj, _ = json.JSONDecoder().raw_decode(text[start:])
        return obj
    except ValueError:
        return {'_text': text[:200], '_status': status}


# --------------------------------------------------------------------------
# The story engine: turns state deltas into sentences
# --------------------------------------------------------------------------

EVENT_SENTENCES = {
    # One entry per kind in the firmware's System_EventCatalogRows.h. {subject}/{detail}
    # are dropped cleanly when the device leaves them empty (see event_sentence).
    # --- network ---
    'wifi_connected': ('radio', 'Wi-Fi joined {subject}'),
    'wifi_disconnected': ('radio', 'Wi-Fi dropped {subject}'),
    'wifi_connect_failed': ('radio', 'Wi-Fi could not join {subject}'),
    'wifi_net_added': ('radio', 'Wi-Fi network saved: {subject}'),
    'wifi_net_removed': ('radio', 'Wi-Fi network forgotten: {subject}'),
    'time_synced': ('system', 'Clock synced from {subject}'),
    'mqtt_connected': ('radio', 'MQTT broker connected'),
    'mqtt_disconnected': ('radio', 'MQTT broker lost'),
    'mqtt_start_failed': ('radio', 'MQTT could not start: {subject}'),
    'mqtt_ext_sensor_new': ('radio', 'New external sensor seen over MQTT: {subject}'),
    'http_server_started': ('radio', 'Web server up ({subject}, port {detail})'),
    'http_server_stopped': ('radio', 'Web server stopped'),
    'cert_generated': ('radio', 'TLS certificate generated for {subject}'),
    'ble_on': ('radio', 'Bluetooth stack started'),
    'ble_off': ('radio', 'Bluetooth stack stopped'),
    'ble_connected': ('radio', 'Bluetooth link up: {subject}'),
    'ble_disconnected': ('radio', 'Bluetooth link down: {subject}'),
    'ble_stack_recycled': ('radio', 'Bluetooth stack restarted to clear a wedge: {subject}'),
    'phone_link_up': ('wear', 'Phone link up ({detail})'),
    'phone_link_down': ('wear', 'Phone link down ({detail})'),
    # --- glasses and ring ---
    'g2_connected': ('wear', 'Glasses connected: {subject}'),
    'g2_disconnected': ('wear', 'Glasses disconnected: {subject} ({detail})'),
    'g2_reconnect_failed': ('wear', 'Glasses reconnect failed ({subject}): {detail}'),
    # The firmware infers these from the temple's app plugin answering (or not
    # answering) renderer heartbeats; the G2 wear sensor itself is not decoded.
    # Say what was measured, not what it is taken to mean.
    'g2_plugin_alive': ('wear', 'Glasses {subject} temple app answering again'),
    'g2_plugin_silent': ('wear', 'Glasses {subject} temple app went quiet (idle, or set down)'),
    'g2_hijack_entered': ('wear', 'Glasses app opened: {subject}'),
    'g2_hijack_exited': ('wear', 'Glasses app closed: {subject}'),
    'g2_long_press': ('wear', 'Long press ({subject}) {detail}'),
    'g2_long_release': ('wear', 'Long press released ({subject})'),
    'g2_silent_mode': ('wear', 'Glasses silent mode {subject}'),
    'ring_connected': ('wear', 'Ring connected: {subject}'),
    'ring_disconnected': ('wear', 'Ring disconnected: {subject}'),
    'ring_reconnect_failed': ('wear', 'Ring reconnect failed: {detail}'),
    'ring_worn': ('wear', 'Ring on the finger'),
    'ring_not_worn': ('wear', 'Ring taken off'),
    # --- accounts and security ---
    'login_ok': ('auth', '{subject} signed in via {detail}'),
    'login_fail': ('auth', 'Failed sign-in for {subject} via {detail}'),
    'logout': ('auth', '{subject} signed out'),
    'login_locked': ('security', 'Sign-in locked for {subject} after repeated failures, {detail} s'),
    'user_request': ('auth', 'Account requested: {subject}'),
    'user_added': ('auth', 'Account created: {subject}, by {detail}'),
    'user_approved': ('auth', 'Account approved: {subject}, by {detail}'),
    'user_rejected': ('auth', 'Account request denied: {subject}'),
    'user_deleted': ('auth', 'Account deleted: {subject}, by {detail}'),
    'password_changed': ('auth', 'Password changed for {subject} ({detail})'),
    'command_denied': ('security', '{subject} was refused a privileged command: {detail}'),
    'auth_db_fault': ('security', 'The account database failed to load'),
    'user_promoted': ('security', '{subject} granted admin'),
    'user_demoted': ('security', 'Admin revoked from {subject}'),
    'user_banned': ('security', 'Account suspended: {subject}'),
    'ip_banned': ('security', 'Address banned: {subject}'),
    'peer_unpaired': ('security', 'Peer removed from the trust list: {subject}'),
    'identity_regenerated': ('security', 'Device identity key rotated; every bond must pair again'),
    # --- power ---
    'usb_on': ('power', 'USB power plugged in'),
    'usb_off': ('power', 'Running on battery'),
    'charging_started': ('power', 'Battery charging'),
    'charging_stopped': ('power', 'Charging stopped'),
    'battery_low': ('power', 'Battery low'),
    'battery_critical': ('power', 'Battery critical'),
    'battery_full': ('power', 'Battery full'),
    'power_mode_changed': ('power', 'Power mode: {subject} {detail}'),
    'power_save_enter': ('power', 'Entering power save'),
    'power_save_exit': ('power', 'Leaving power save'),
    # --- sensors ---
    'sensor_started': ('sense', 'Sensor started: {subject}'),
    'sensor_stopped': ('sense', 'Sensor stopped: {subject}'),
    'sensor_start_failed': ('sense', 'Sensor failed to start: {subject}'),
    'sensor_fault': ('sense', 'Sensor fault: {subject} {detail}'),
    'sensor_unresponsive': ('sense', 'Sensor unresponsive: {subject}'),
    'sensor_recovered': ('sense', 'Sensor recovered: {subject}'),
    'gps_fix': ('sense', 'GPS fix acquired'),
    'gps_lost': ('sense', 'GPS fix lost'),
    'button': ('sense', 'Button: {subject}'),
    'presence_detected': ('sense', 'Presence detected ({subject})'),
    'presence_cleared': ('sense', 'Presence gone'),
    'motion_detected': ('sense', 'Motion {subject}'),
    'gesture': ('sense', 'Gesture: swipe {subject}'),
    'imu_shake': ('sense', 'Shake felt ({subject})'),
    'imu_tap': ('sense', 'Tap felt ({subject})'),
    'imu_freefall': ('sense', 'Free fall for {subject}'),
    'imu_orientation': ('sense', 'Turned to {subject}, was {detail}'),
    'imu_walking': ('sense', 'Walking {subject} {detail}'),
    'tof_object_detected': ('sense', 'Object {subject} at {detail} mm'),
    'thermal_hot_alert': ('sense', 'Hot spot seen by the thermal camera: {subject} °C'),
    'fm_tuned': ('sense', 'FM radio tuned to {subject} MHz'),
    'fm_rds_station': ('sense', 'FM station identified: {subject} on {detail}'),
    'rtc_power_loss': ('sense', 'Real-time clock lost power; its time is not trusted'),
    # --- camera, microphone, notes, on-device AI ---
    'photo_saved': ('media', 'Photo saved: {subject}'),
    'video_record_started': ('media', 'Video recording started: {subject}'),
    'video_saved': ('media', 'Video saved: {subject}, {detail} frames'),
    'video_record_failed': ('media', 'Video clip did not finalize: {subject}'),
    'mic_record_started': ('media', 'Microphone recording started: {subject}'),
    'mic_saved': ('media', 'Recording saved: {subject}'),
    'ei_continuous_started': ('media', 'Camera AI detection running continuously'),
    'ei_detected': ('media', 'Camera AI sees {subject} ({detail})'),
    'ei_lost': ('media', 'Camera AI lost {subject}'),
    'note_started': ('voice', 'Note recording started via {subject}'),
    'note_saved': ('voice', 'Note saved'),
    'note_failed': ('voice', 'Note not saved: {subject}'),
    'voice_wake': ('voice', 'Wake word heard'),
    'voice_command': ('voice', 'Voice command: {subject}'),
    'voice_armed': ('voice', 'Voice commands armed as {subject}'),
    'voice_disarmed': ('voice', 'Voice commands disarmed'),
    'llm_model_loaded': ('voice', 'Language model loaded: {subject}'),
    'llm_model_unloaded': ('voice', 'Language model unloaded; assistant offline'),
    'llm_load_failed': ('voice', 'Language model failed to load: {subject}'),
    'llm_state_corrupt': ('voice', 'Language model engine fault: {subject}'),
    'llm_gen_done': ('voice', 'Model finished ({subject}, {detail})'),
    'llm_request': ('voice', 'Question sent to the {subject} assistant ({detail})'),
    'stt_started': ('voice', 'Transcription started ({subject})'),
    'stt_stopped': ('voice', 'Transcription {subject}'),
    'stt_segment': ('voice', 'Speech segment {subject} heard ({detail}), transcribing'),
    'stt_speaker': ('voice', 'Speaker {subject} recognised for segment {detail}'),
    # --- mesh ---
    'peer_online': ('mesh', 'Mesh peer online: {subject}'),
    'peer_offline': ('mesh', 'Mesh peer offline: {subject}'),
    'peer_paired': ('mesh', 'Paired with {subject}'),
    'pair_window_open': ('mesh', 'Pairing window open for {subject} s'),
    'pair_window_closed': ('mesh', 'Pairing window closed'),
    'bond_online': ('mesh', 'Bonded peer online: {subject}'),
    'bond_offline': ('mesh', 'Bonded peer silent: {subject}'),
    'bond_reject': ('security', 'Unpaired device probed the bond channel ({detail} times)'),
    'mesh_promoted': ('mesh', 'Promoted to backup mesh master'),
    'mesh_demoted': ('mesh', 'Mesh master is back; stepped down'),
    'mesh_passphrase_changed': ('mesh', 'Mesh group key {detail}'),
    'text_rx': ('mesh', 'Message from {subject}'),
    'file_rx': ('mesh', 'File received from {subject}'),
    'file_rx_failed': ('mesh', 'File transfer from {subject} failed: {detail}'),
    'remote_cmd_rx': ('mesh', '{subject} ran a command here: {detail}'),
    'remote_cmd_sent': ('mesh', 'Command sent to {subject}: {detail}'),
    'remote_cmd_result': ('mesh', 'Reply from {subject}: {detail}'),
    'remote_stream_started': ('mesh', '{subject} is now watching this console'),
    'espnow_on': ('mesh', 'ESP-NOW radio on'),
    'espnow_off': ('mesh', 'ESP-NOW radio off'),
    # --- automation ---
    'automation_fired': ('auto', 'Automation fired: {subject}'),
    'automation_added': ('auto', 'Automation added: {subject}'),
    'automation_deleted': ('auto', 'Automation deleted: {subject}'),
    'automation_action_dropped': ('auto', 'Automation {subject} skipped an action: {detail}'),
    # --- system ---
    'setting_changed': ('system', 'Setting changed: {subject}'),
    'feature_toggled': ('system', 'Feature {subject} turned {detail}'),
    'boot_started': ('system', 'Booting'),
    'boot_finished': ('system', 'Boot complete ({detail})'),
    'reboot': ('system', 'Rebooting ({subject})'),
    'crash': ('system', 'Crash recorded'),
    'factory_reset': ('system', 'Factory reset by {subject}'),
    'backup_created': ('system', 'Config backup exported: {subject}'),
    'backup_restored': ('system', 'Config backup restored: {subject}'),
    'config_file_corrupt': ('system', 'Config file failed its integrity check: {subject}'),
    'secret_decrypt_failed': ('security', 'A stored secret could not be decrypted: {subject}'),
    'display_init_failed': ('system', 'Display did not initialize (address {subject})'),
    # --- firmware updates ---
    'ota_upload_started': ('system', 'Firmware {subject} upload started ({detail} bytes)'),
    'ota_upload_finished': ('system', 'Firmware {subject} upload finished: {detail}'),
    'ota_staged': ('system', 'Firmware {subject} verified and staged ({detail})'),
    'ota_stage_rejected': ('system', 'Firmware {subject} rejected: {detail}'),
    'ota_trial_started': ('system', 'Trial boot of firmware {subject}; proving itself before it is kept'),
    'ota_accepted': ('system', 'Firmware {subject} accepted ({detail})'),
    'ota_rolled_back': ('system', 'Firmware rolled back: {subject} ({detail})'),
    'ota_result': ('system', 'Firmware update {subject}: {detail}'),
    'ota_recovery_entered': ('system', 'Entered the recovery updater ({subject})'),
    'ota_credential_changed': ('security', 'Recovery credential {subject} by {detail}'),
    'firmware_changed': ('system', 'Running new firmware: {subject}'),
    'heap_low': ('system', 'Device memory running low: {subject} B free ({detail})'),
    # --- storage ---
    'sd_mounted': ('store', 'SD card mounted ({detail})'),
    'sd_unmounted': ('store', 'SD card unmounted'),
    'sd_write_failed': ('store', 'SD card stopped accepting writes'),
    'sd_write_recovered': ('store', 'SD card writes resumed'),
    'file_deleted': ('store', 'File deleted: {subject}'),
    'settings_save_failed': ('store', 'Settings could not be saved ({subject}: {detail})'),
    'storage_formatted': ('store', 'Storage wiped: {subject}'),
    'fs_low_space': ('store', 'Storage running low'),
    # --- companion radio chip ---
    'companion_online': ('radio', 'Radio companion online ({subject}, {detail})'),
    'companion_offline': ('radio', 'Radio companion offline: {subject}; {detail}'),
    'companion_restarted': ('radio', 'Radio companion restarted ({subject}, {detail})'),
    'companion_mismatch': ('radio', 'Radio companion version mismatch: runs {subject}, expected {detail}'),
    'companion_low_memory': ('radio', 'Radio companion low on memory: {subject} free'),
    'companion_updated': ('radio', 'Radio companion firmware {detail}: {subject}'),
    'companion_reboot': ('system', 'Rebooting because the radio companion stayed down: {detail}'),
}

KIND_CATEGORY_HINTS = (('wifi', 'radio'), ('mqtt', 'radio'), ('ble', 'radio'), ('g2', 'wear'), ('ring', 'wear'),
                       ('phone', 'wear'), ('login', 'auth'), ('logout', 'auth'), ('battery', 'power'),
                       ('charg', 'power'), ('power', 'power'), ('usb', 'power'), ('sensor', 'sense'),
                       ('imu', 'sense'), ('gps', 'sense'), ('peer', 'mesh'), ('espnow', 'mesh'), ('bond', 'mesh'),
                       ('voice', 'voice'), ('llm', 'voice'), ('automation', 'auto'), ('ota', 'system'),
                       ('sd_', 'store'), ('fs_', 'store'), ('companion', 'radio'))

OWN_POLL_COMMANDS = {'events', 'memreport json', 'perftop json', 'taskstats json', 'espnowmeshstatus', 'g2status',
                     'status json', 'power json', 'batterystatus json', 'blestatus json', 'sensors json brief', 'whoami',
                     'ring status json', 'ring health json'}

EVENT_LINE = re.compile(r'^\s*\[(\d+)\]\s+(\d+)s ago\s+(\S+)\s*(.*)$')


def event_sentence(ev):
    kind = ev['kind']
    cat, tmpl = EVENT_SENTENCES.get(kind, (None, None))
    if cat is None:
        cat = 'system'
        for needle, c in KIND_CATEGORY_HINTS:
            if needle in kind:
                cat = c
                break
        words = kind.replace('_', ' ')
        tmpl = words.capitalize() + (': {subject}' if ev.get('subject') else '')
    for field in ('subject', 'detail', 'source'):
        if not ev.get(field):
            # drop the placeholder with whatever introduced it: a preposition, a
            # colon/comma/semicolon, or the parentheses around it
            ph = r'\{%s\}' % field
            units = r'(?:\s*(?:s|mm|MHz|°C|bytes|tokens|frames|free|times))?'
            tmpl = re.sub(r'\s*\([^()]*' + ph + r'[^()]*\)', '', tmpl)                 # "(x {f} y)" goes whole
            tmpl = re.sub(r'\s+(?:from|to|for|with|via|by|on|at|as|after|because|was|runs|expected)\s+' + ph + units, '', tmpl)
            tmpl = re.sub(r'[,:;]?\s*' + ph + units, '', tmpl)
    text = tmpl.format(subject=ev.get('subject') or '', detail=ev.get('detail') or '', source=ev.get('source') or '')
    text = re.sub(r'\(\s*,\s*', '(', text)          # "(, 12 tokens)" -> "(12 tokens)"
    text = re.sub(r',\s*\)', ')', text)
    text = re.sub(r'\(\s*\)', '', text)
    text = re.sub(r'\s+', ' ', text).strip(' ,;:')
    if tmpl.startswith('{subject}') and ev.get('subject'):
        return cat, text  # a username or device name keeps its own casing
    return cat, (text[:1].upper() + text[1:]) if text else kind


def rssi_bucket(rssi):
    if rssi is None:
        return None
    if rssi >= -55:
        return 'strong'
    if rssi >= -67:
        return 'good'
    if rssi >= -75:
        return 'fair'
    return 'weak'


class Story:
    """Keeps the last known facts and emits a sentence when one changes."""

    def __init__(self):
        self.facts = {}
        self.moments = deque(maxlen=MOMENTS_LEN)
        self.last_event_seq = None
        self.console_seen = deque(maxlen=400)

    def add(self, cat, text, source='delta'):
        if self.moments and self.moments[-1]['text'] == text and time.time() - self.moments[-1]['t'] < 30:
            return
        self.moments.append({'t': time.time(), 'cat': cat, 'text': text, 'src': source})

    def observe(self, key, value, on_change, first=False):
        """on_change(old, new) -> (cat, text) or None."""
        old = self.facts.get(key, Ellipsis)
        self.facts[key] = value
        if old is Ellipsis:
            if first:
                res = on_change(None, value)
                if res:
                    self.add(*res)
            return
        if old != value:
            res = on_change(old, value)
            if res:
                self.add(*res)

    def ingest_events(self, events):
        """events: newest first, from the device ring."""
        if not events:
            return
        if self.last_event_seq is None:
            self.last_event_seq = events[0]['seq']
            # Show the most recent few as backstory on first contact.
            for ev in reversed(events[:5]):
                cat, text = event_sentence(ev)
                self.moments.append({'t': time.time() - ev['ago'], 'cat': cat, 'text': text, 'src': 'event'})
            return
        if events[0]['seq'] < self.last_event_seq:
            # the sequence restarted: the device rebooted and every row is new
            self.last_event_seq = 0
        fresh = [ev for ev in events if ev['seq'] > self.last_event_seq]
        for ev in reversed(fresh):
            if ev['kind'].startswith('stt_'):
                self.facts['sttEvents'] = True  # firmware narrates speech itself; stop scraping TL log lines
            cat, text = event_sentence(ev)
            self.moments.append({'t': time.time() - ev['ago'], 'cat': cat, 'text': text, 'src': 'event'})
        if fresh:
            self.last_event_seq = max(ev['seq'] for ev in fresh)


def parse_events_text(text):
    events = []
    for line in text.splitlines():
        m = EVENT_LINE.match(line)
        if not m:
            continue
        seq, ago, kind, rest = m.groups()
        rest, by = rest.rstrip(), ''
        idx = rest.rfind('  by ')
        if idx >= 0:
            by, rest = rest[idx + 5:].strip(), rest[:idx]
        elif rest.startswith('by '):
            by, rest = rest[3:].strip(), ''
        subject, detail = rest.strip(), ''
        dm = re.match(r'^(.*?)\s*\((.*)\)$', subject)
        if dm:
            subject, detail = dm.group(1).strip(), dm.group(2).strip()
        source, who = (by.split(':', 1) + [''])[:2] if by else ('', '')
        events.append({'seq': int(seq), 'ago': int(ago), 'kind': kind, 'subject': subject,
                       'detail': detail, 'source': source, 'who': who})
    return events


# --------------------------------------------------------------------------
# Poller
# --------------------------------------------------------------------------

class Poller(threading.Thread):
    def __init__(self, transport, args):
        super().__init__(daemon=True)
        self.tr = transport
        self.args = args
        self.lock = threading.Lock()
        self.story = Story()
        self.state = {'system': None, 'power': None, 'battery': None, 'ble': None, 'sensors': None,
                      'mem': None, 'perf': None, 'tasks': None, 'mesh': None, 'events': [], 'console': []}
        self.series = {k: deque(maxlen=SERIES_LEN) for k in ('heapFree', 'psramFree', 'cpuMhz', 'rssi', 'busy', 'battery', 'flashUsed', 'sdUsed')}
        self.trail = {}          # task name -> deque of (t, level, span, pct); insertion order = row order
        self.tool = {'connected': False, 'transport': transport.name, 'target': transport.target,
                     'lastError': '', 'polls': 0, 'badPolls': 0, 'ts': 0, 'started': time.time()}
        self.camera = {'available': False, 'ts': 0, 'bytes': None}
        self.due = {}
        self.unavailable = set()
        self.console_lines = deque(maxlen=CONSOLE_LEN)
        self._console_prev = []
        self.stop = threading.Event()

    # Lane scheduling ------------------------------------------------------
    def _due(self, lane, period):
        now = time.monotonic()
        if now >= self.due.get(lane, 0):
            self.due[lane] = now + period
            return True
        return False

    def run(self):
        while not self.stop.is_set():
            t0 = time.monotonic()
            try:
                self.poll_once()
                self.tool['connected'] = True
                self.tool['lastError'] = ''
            except TransportError as e:
                self.tool['connected'] = False
                self.tool['lastError'] = str(e)
                self.story.observe('link', False, lambda o, n: ('tool', 'Lost contact with the device'))
                time.sleep(2.0)
                if hasattr(self.tr, 'reopen'):
                    try:
                        self.tr.reopen()
                        self.story.observe('link', True, lambda o, n: ('tool', 'Serial port back; signed in again'))
                    except TransportError as e2:
                        self.tool['lastError'] = str(e2)
            except Exception as e:  # keep the page alive on any parse surprise
                self.tool['lastError'] = f'{type(e).__name__}: {e}'
            self.tool['polls'] += 1
            self.tool['ts'] = time.time()
            elapsed = time.monotonic() - t0
            time.sleep(max(0.2, self.args.interval - elapsed))

    def poll_once(self):
        a = self.args
        fast = self._due('fast', a.interval)
        mid = self._due('mid', max(a.interval, 5.0))
        slow = self._due('slow', max(a.interval, 12.0))
        updates = {}
        if fast:
            sysdoc = self.tr.get_json('/api/system')
            # A reply that is not the status document (timeout, a log line in the
            # middle of the JSON, a truncated blob) must not replace the last good
            # one: an empty 'net' block would read as Wi-Fi dropping, mesh off, ...
            if isinstance(sysdoc, dict) and ('net' in sysdoc or 'connectivity' in sysdoc):
                updates['system'] = sysdoc
            else:
                self.tool['badPolls'] = self.tool.get('badPolls', 0) + 1
            self.story.observe('link', True, lambda o, n: ('tool', 'Connected to the device'), first=True)
        if mid:
            for key, path in (('power', '/api/power/status'), ('battery', '/api/battery/status'),
                              ('ble', '/api/ble/status'), ('sensors', '/api/sensors/status')):
                if path in self.unavailable:
                    continue
                doc = self.tr.get_json(path)
                if isinstance(doc, dict) and (doc.get('_unavailable') or doc.get('_status') in (404, 501)):
                    self.unavailable.add(path)
                updates[key] = doc
                if key == 'ble' and isinstance(doc, dict) and (doc.get('ring') or {}).get('connected'):
                    # battery, wear and charge live in the ring's health view, not its link status
                    rh = self.tr.command_json('ring health json')
                    if isinstance(rh, dict) and rh.get('schema'):
                        doc['ring'].update({
                            'battery': rh.get('battery') if rh.get('batteryValid') else None,
                            'batteryAgeSec': rh.get('batteryAgeSec'),
                            'worn': rh.get('wearName') if rh.get('wearValid') else None,
                            'charge': rh.get('charge') if rh.get('chargeValid') else None,
                        })
            status, text = self.tr.command('events', capture=True)
            updates['events'] = parse_events_text(text)[:EVENTS_LEN]
            self._poll_console()
        cmds = []
        if fast:
            cmds.append('memreport json')
            if 'taskstats json' not in self.unavailable:
                cmds.append('taskstats json')  # cheap snapshot; feeds the activity trail
            if 'perftop json' not in self.unavailable:
                cmds.append('perftop json')  # reads the device's shared sampler; nothing blocks
        if slow:
            pass
            sysd = updates.get('system') or self.state.get('system') or {}
            espnow = ((sysd.get('connectivity') or {}).get('espnow') or {})
            if espnow.get('running'):
                cmds.append('espnowmeshstatus')
        if cmds:
            results = self.tr.batch_json(cmds)
            for c, r in zip(cmds, results):
                key = {'memreport json': 'mem', 'perftop json': 'perf', 'taskstats json': 'tasks',
                       'espnowmeshstatus': 'mesh'}[c]
                if isinstance(r, dict) and r.get('_unavailable'):
                    self.unavailable.add(c)
                updates[key] = r
        if a.camera and self._due('camera', 1.0):
            self._poll_camera()
        with self.lock:
            self.state.update(updates)
            self._derive(fresh=set(updates))

    def _poll_console(self):
        status, text = self.tr.get_text('/api/cli/logs')
        if status != 200:
            return
        lines = [ln.rstrip() for ln in text.splitlines() if ln.strip()]
        # The device returns a whole ring each time; append only what is new.
        prev = self._console_prev
        new = lines
        if prev:
            # find the longest suffix of prev that is a prefix of lines
            for k in range(min(len(prev), len(lines)), 0, -1):
                if prev[-k:] == lines[:k]:
                    new = lines[k:]
                    break
            else:
                new = [ln for ln in lines if ln not in prev[-40:]]
        self._console_prev = lines
        for ln in new:
            self.console_lines.append(ln[:240])

    def _poll_camera(self):
        sysd = self.state.get('sensors') or {}
        if not (sysd.get('cameraConnected') or sysd.get('cameraRunning') or sysd.get('cameraEnabled')):
            self.camera['available'] = False
            return
        status, body = self.tr.get_bytes('/api/sensors/camera/frame')
        if status == 200 and body[:2] == b'\xff\xd8':
            self.camera.update(available=True, ts=time.time(), bytes=body)
        else:
            self.camera['available'] = False

    # Derivations -----------------------------------------------------------
    def _derive(self, fresh=()):
        s = self.state
        st = self.story
        now = time.time()
        sysd = s.get('system') or {}
        net = sysd.get('net') or {}
        conn = sysd.get('connectivity') or {}
        mem = sysd.get('mem') or {}
        memr = s.get('mem') or {}
        power = s.get('power') or {}
        bat = s.get('battery') or {}
        ble = s.get('ble') or {}
        sens = s.get('sensors') or {}
        perf = s.get('perf') or {}

        # series
        heap_free = (memr.get('dram') or {}).get('free')
        if heap_free is None and mem.get('heap_free_kb') is not None:
            heap_free = mem['heap_free_kb'] * 1024
        if heap_free is not None:
            prev = self.series['heapFree'][-1] if self.series['heapFree'] else None
            self.series['heapFree'].append((now, heap_free))
            # A sudden internal-RAM dip is worth a line with its likely cause: the
            # non-idle tasks seen on a core in the last few seconds and the most
            # recent device event. Answers "what was that dip?" without a debugger.
            if prev and prev[1] - heap_free >= 16 * 1024 and now - st.facts.get('lastHeapDip', 0) > 10:
                st.facts['lastHeapDip'] = now
                busy = sorted({n for n, d in self.trail.items() if n not in ('IDLE0', 'IDLE1', 'cmd_exec')
                               for t, lvl, sp, pct in list(d)[-4:] if lvl > 0 and now - t < 8})
                recent = [ev for ev in (s.get('events') or []) if ev.get('ago', 999) < 12]
                why = []
                if busy:
                    why.append('active: ' + ', '.join(busy[:5]))
                if recent:
                    why.append('just after ' + event_sentence(recent[0])[1].lower())
                st.add('memory', f'Internal RAM dipped {(prev[1] - heap_free) // 1024} KB to {heap_free // 1024} KB'
                       + (' (' + '; '.join(why) + ')' if why else ''))
        ps_free = (memr.get('psram') or {}).get('free')
        if ps_free is None and mem.get('psram_free_kb') is not None:
            ps_free = mem['psram_free_kb'] * 1024
        if ps_free:
            self.series['psramFree'].append((now, ps_free))
        stor = sysd.get('storage') or {}
        if stor.get('used_kb') is not None:
            self.series['flashUsed'].append((now, stor['used_kb'] * 1024))
        sd = stor.get('sd') or {}
        if sd.get('used_mb') is not None:
            self.series['sdUsed'].append((now, sd['used_mb'] * 1024 * 1024))
        if power.get('cpuMhz') is not None:
            self.series['cpuMhz'].append((now, power['cpuMhz']))
        if net.get('rssi') is not None and net.get('wifiConnected'):
            self.series['rssi'].append((now, net['rssi']))
        if perf.get('cpuValid'):
            idle = sum(t.get('cpuPct', 0) for t in perf.get('tasks', []) if t.get('idle'))
            cores = max(1, sum(1 for t in perf.get('tasks', []) if t.get('idle')))
            self.series['busy'].append((now, max(0, 100 - idle / cores)))
        if isinstance(bat.get('percentage'), (int, float)):
            self.series['battery'].append((now, bat['percentage']))
        if 'tasks' in fresh:
            # samples are (t, level, span_s, cpuPct): a state snapshot covers one poll
            # period; a measured CPU share covers the whole perf period it stands for.
            rows = [t for t in (s.get('tasks') or {}).get('tasks') or [] if t.get('name')]
            levels = {t['name']: [{'RUN': 2, 'READY': 1}.get(t.get('state'), 0), self.args.interval, None] for t in rows}
            # FreeRTOS marks only the sampling core's current task as running; the
            # task on the other core is listed as READY. The executor that takes the
            # sample is pinned to core 0, so core 1's runner is the highest-priority
            # ready task that is not core 0's idle task. Level 3 = inferred running.
            running = [t['name'] for t in rows if t.get('state') == 'RUN']
            if len(running) == 1:
                has_core = any('core' in t for t in rows)
                # with core affinity reported, only tasks that may run on core 1 qualify
                ready = [t for t in rows if t.get('state') == 'READY'
                         and (t.get('core') != 0 if has_core else t['name'] != 'IDLE0')]
                if ready:
                    other = max(ready, key=lambda t: (t.get('prio') or 0, t['name'] == 'IDLE1'))['name']
                    levels[other][0] = 3
            if 'perf' in fresh:
                for t in perf.get('tasks') or []:
                    pm = t.get('cpuPermille')
                    if pm is None:
                        pm = (t.get('cpuPct') or 0) * 10  # older firmware: whole percent only
                    if t.get('name') in levels and not t.get('idle') and pm > 0:
                        span = (perf.get('windowMs') or 500) / 1000.0
                        levels[t['name']] = [max(levels[t['name']][0], 2 if pm >= 250 else 1), span, pm / 10]
            for name, (lvl, span, pct) in levels.items():
                self.trail.setdefault(name, deque(maxlen=TRAIL_LEN)).append((now, lvl, span, pct))

        # deltas -> moments
        st.observe('wifi', (bool(net.get('wifiConnected')), net.get('ssid') or ''),
                   lambda o, n: ('radio', f"Wi-Fi joined {n[1]}") if n[0] else
                   (('radio', 'Wi-Fi disconnected') if o and o[0] else None))
        st.observe('rssiBucket', rssi_bucket(net.get('rssi')) if net.get('wifiConnected') else None,
                   lambda o, n: ('radio', f'Wi-Fi signal now {n} ({net.get("rssi")} dBm)') if n and o else None)
        st.observe('cpuMhz', power.get('cpuMhz'),
                   lambda o, n: ('brain', f'CPU clock {"raised" if n > o else "lowered"} to {n} MHz'
                                 + (' for busy work' if n > o else ', back to idle pace')) if o and n else None)
        st.observe('powerMode', power.get('modeName'),
                   lambda o, n: ('power', f'Power mode switched to {n}') if o and n else None)
        if heap_free is not None:
            st.observe('heapBucket', int(heap_free // 20480),
                       lambda o, n: ('memory', f'Internal RAM {"freed up" if n > o else "tightened"}: {heap_free // 1024} KB free')
                       if o is not None else None)
        g2 = (ble.get('g2') or {}).get('connection') or {}
        g2c = bool(conn.get('bluetooth', {}).get('g2Connected')) or (g2.get('state') == 'connected')
        st.observe('g2', g2c, lambda o, n: ('wear', 'Glasses connected' if n else 'Glasses disconnected') if o is not None else None)
        st.observe('headUp', ((ble.get('g2') or {}).get('controls') or {}).get('headUp'),
                   lambda o, n: ('wear', 'Head-up gesture on the glasses') if n and o is not None else None)
        st.observe('ring', bool((ble.get('ring') or {}).get('connected')),
                   lambda o, n: ('wear', 'Ring connected' if n else 'Ring disconnected') if o is not None else None)
        pl = ble.get('phoneLink') or {}
        st.observe('phone', pl.get('connected'),
                   lambda o, n: ('wear', 'Phone link up' if n else 'Phone link down') if o is not None else None)
        st.observe('bleState', (conn.get('bluetooth') or {}).get('state'),
                   lambda o, n: ('radio', f'Bluetooth role: {n}') if o and n else None)
        st.observe('charging', bat.get('charging'),
                   lambda o, n: ('power', 'Battery charging' if n else 'Running on battery') if o is not None and n is not None else None)
        st.observe('mesh', ((conn.get('espnow') or {}).get('running')),
                   lambda o, n: ('mesh', 'ESP-NOW mesh radio on' if n else 'ESP-NOW mesh radio off')
                   if o is not None and n is not None else None)
        mesh = s.get('mesh') or {}
        alive = [p for p in (mesh.get('peers') or []) if p.get('alive')]
        st.observe('peersAlive', len(alive),
                   lambda o, n: ('mesh', f'{n} mesh peer{"s" if n != 1 else ""} alive') if o is not None else None)
        running = sorted(k[:-7] for k, v in sens.items() if k.endswith('Running') and v)
        st.observe('sensorsRunning', tuple(running),
                   lambda o, n: self._sensor_delta(o, n))
        up = sysd.get('uptime_hms') or ''
        um = re.match(r'(?:(\d+)d\s*)?(?:(\d+)h\s*)?(?:(\d+)m\s*)?(?:(\d+)s)?$', up.strip())
        upsec = sum(int(x or 0) * k for x, k in zip(um.groups(), (86400, 3600, 60, 1))) if um and up else None
        if upsec is not None:
            prev = st.facts.get('uptimeSec')
            st.facts['uptimeSec'] = upsec
            if prev is not None and upsec < prev:
                st.add('system', f"Device restarted ({sysd.get('reset_reason') or 'reason unknown'}), uptime reset")
        st.observe('sessions', (conn.get('webserver') or {}).get('sessions'),
                   lambda o, n: ('auth', f'{n} web session{"s" if n != 1 else ""} open') if o is not None and n is not None else None)
        st.ingest_events(s.get('events') or [])

        # serial transports expose their own console ring
        if hasattr(self.tr, 'console') and self.tr.name == 'serial':
            for ln in list(self.tr.console):
                if ln not in self.console_lines:
                    self.console_lines.append(ln[:240])
        # console lines that read as moments
        for ln in list(self.console_lines)[-120:]:
            if ln in st.console_seen:
                continue
            st.console_seen.append(ln)
            body = re.sub(r'^\[\d+\]\s*', '', ln)
            m = re.match(r'^\[(web|serial|ble|oled|g2|voice)\]\s+(.*)$', body)
            if m and ('Login successful' in body or 'logged in' in body):
                st.add('auth', f'Sign-in over {m.group(1)}', 'console')
            cm = re.match(r'^\[CMD\]\s*(\S+?)@(\w+):\s*(.*?)(?:\s*->\s*(.*))?$', body)
            if cm:
                user, via, cmdline, result = cm.groups()
                verb = cmdline.split(' ')[0]
                if cmdline not in OWN_POLL_COMMANDS and verb not in ('login',):
                    st.add('cmd', f'{user} ran "{cmdline[:48]}" over {via}' + (f' -> {result[:30]}' if result and not result.startswith('OK') and not result.startswith('{') else ''), 'console')
            self._voice_line(body, now)
        self.state['console'] = list(self.console_lines)[-CONSOLE_LEN:]

    def _voice_line(self, body, now):
        st = self.story
        vm = re.search(r'STT: TL (capture_start|warm|boundary|fe_end|spk)\b(.*)$', body)
        if not vm:
            return
        what, rest = vm.groups()
        self.state['voiceSeenAt'] = now
        if st.facts.get('sttEvents') and what != 'warm':
            return  # covered by stt_started/stt_segment/stt_speaker/stt_stopped events
        if what == 'capture_start':
            st.add('voice', 'Listening: speech capture started', 'console')
        elif what == 'warm':
            st.add('voice', 'Speech model warmed up', 'console')
        elif what == 'fe_end':
            st.add('voice', 'Speech capture ended', 'console')
        elif what == 'spk':
            sm = re.search(r'spk=(\d+) sim=([\d.]+) new=(\d)', rest)
            if sm and now - st.facts.get('lastSpkMoment', 0) > 20:
                st.facts['lastSpkMoment'] = now
                st.add('voice', f'{"New speaker enrolled" if sm.group(3) == "1" else "Speaker " + sm.group(1) + " recognised"} (match {float(sm.group(2)):.2f})', 'console')
        elif what == 'boundary':
            vv = re.search(r'vad_voiced=(\d+)', rest)
            secs = int(vv.group(1)) / 16000 if vv else None  # voiced samples at 16 kHz
            if now - st.facts.get('lastVoiceMoment', 0) > 8:
                st.facts['lastVoiceMoment'] = now
                st.add('voice', f'Speech heard{f" ({secs:.1f} s of voice)" if secs is not None else ""}, transcribing on-device', 'console')

    @staticmethod
    def _sensor_delta(old, new):
        if old is None:
            return None
        started = [x for x in new if x not in old]
        stopped = [x for x in old if x not in new]
        parts = []
        if started:
            parts.append('started ' + ', '.join(started))
        if stopped:
            parts.append('stopped ' + ', '.join(stopped))
        return ('sense', 'Sensors ' + '; '.join(parts)) if parts else None

    def snapshot(self):
        with self.lock:
            out = {k: v for k, v in self.state.items()}
            out['series'] = {k: [[round(t, 1), v] for t, v in list(d)[-SERIES_LEN:]] for k, d in self.series.items()}
            out['moments'] = list(self.story.moments)
            # every task, so the rows stacked together account for both cores at each sample
            out['trail'] = {n: [[round(t, 1), v, sp, pct] for t, v, sp, pct in d if v > 0] for n, d in self.trail.items()}
            out['tool'] = dict(self.tool)
            if hasattr(self.tr, 'console'):
                out['tool']['consoleRing'] = len(self.tr.console)
            out['camera'] = {'available': self.camera['available'], 'ts': self.camera['ts']}
            out['now'] = time.time()
            return out


# --------------------------------------------------------------------------
# Local web server
# --------------------------------------------------------------------------

def make_handler(poller):
    page = HERE / 'showcase.html'

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def _send(self, code, ctype, body, cache=False):
            self.send_response(code)
            self.send_header('Content-Type', ctype)
            self.send_header('Content-Length', str(len(body)))
            if not cache:
                self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = urllib.parse.urlparse(self.path).path
            if path in ('/', '/index.html'):
                self._send(200, 'text/html; charset=utf-8', page.read_bytes())
            elif path == '/api/state':
                self._send(200, 'application/json', json.dumps(poller.snapshot()).encode())
            elif path == '/camera.jpg':
                data = poller.camera.get('bytes')
                if data:
                    self._send(200, 'image/jpeg', data)
                else:
                    self._send(404, 'text/plain', b'no frame')
            else:
                self._send(404, 'text/plain', b'not found')

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group()
    src.add_argument('--host', help='device address, e.g. 192.168.1.42 or http://192.168.1.42')
    src.add_argument('--serial', metavar='PORT', help='USB console port instead of Wi-Fi, e.g. /dev/cu.usbmodem14201')
    src.add_argument('--mock', action='store_true', help='rehearse with a fake device')
    ap.add_argument('--user', default=os.environ.get('HW1_USER', 'admin'))
    ap.add_argument('--password', help='device password (or set HW1_PASSWORD; otherwise prompted)')
    ap.add_argument('--interval', type=float, default=2.0, help='fast-lane poll period in seconds (default 2)')
    ap.add_argument('--camera', action='store_true', help='also pull camera frames when the camera is on')
    ap.add_argument('--bind', default='127.0.0.1', help='local address to serve the page on (default localhost only)')
    ap.add_argument('--port', type=int, default=8787)
    ap.add_argument('--baud', type=int, default=115200)
    args = ap.parse_args()

    if args.mock:
        transport = MockTransport()
    else:
        if not args.host and not args.serial:
            ap.error('give --host, --serial or --mock')
        password = args.password or os.environ.get('HW1_PASSWORD') or getpass.getpass(f'Password for {args.user}: ')
        try:
            if args.serial:
                # The port may not exist yet (device unplugged, Mac re-enumerating):
                # wait for it rather than exit, so the tool can be started first.
                said = False
                while True:
                    try:
                        transport = SerialTransport(args.serial, args.user, password, args.baud)
                        break
                    except TransportError as e:
                        if 'not back yet' not in str(e):
                            raise
                        if not said:
                            print(f'waiting for {args.serial}: {e}', file=sys.stderr)
                            said = True
                        time.sleep(3.0)
            else:
                base = args.host if '://' in args.host else 'http://' + args.host
                transport = HttpTransport(base, args.user, password)
                ping = transport.get_json('/api/ping')
                if ping.get('_status'):
                    print(f'warning: /api/ping returned {ping.get("_status")}; is this the device?', file=sys.stderr)
                else:
                    print(f'device: {ping.get("hostname")} fw {ping.get("firmwareVersion")}')
                probe = transport.get_json('/api/system')
                if probe.get('_status') == 401 or probe.get('_forbidden'):
                    print('login refused: check --user/--password (guest accounts cannot run commands)', file=sys.stderr)
        except TransportError as e:
            print(f'cannot reach the device: {e}', file=sys.stderr)
            sys.exit(1)

    poller = Poller(transport, args)
    poller.start()
    server = ThreadingHTTPServer((args.bind, args.port), make_handler(poller))
    url = f'http://{"localhost" if args.bind == "127.0.0.1" else args.bind}:{args.port}/'
    print(f'showcase: {url}   (transport: {transport.name} -> {transport.target})')
    if args.bind != '127.0.0.1':
        print('note: the page is reachable from other machines; it shows device state and console lines')
    # SIGTERM (pkill, a service stop) must reach the finally below, so the
    # serial session is signed out rather than left open on the device.
    import signal
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        poller.stop.set()
        poller.join(timeout=5.0)
        if hasattr(transport, 'close'):
            transport.close()
            print('showcase: signed out of the serial console')


if __name__ == '__main__':
    main()
