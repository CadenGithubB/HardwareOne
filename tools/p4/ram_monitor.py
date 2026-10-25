#!/usr/bin/env python3
"""Live RAM graph through an existing non-resetting serial logger's FIFO/log.

Only sends `memreport json`; never opens USB, logs in, resets or flashes.
Run with --fifo PATH --log PATH --output PRIVATE.jsonl, then open the printed URL.
The logger must timestamp complete lines with HH:MM:SS, as p4log/live_console do.
"""
import argparse
from collections import deque
import json
import os
import re
from pathlib import Path
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def parse_event(line, timestamp):
    labels = (
        ('STT: TL capture_start', 'Speech capture started'),
        ('STT: TL warm ', 'Speech warm-up'),
        ('STT: model ', 'Speech model loaded'),
        ('STT: TL e3 resident=1', 'Resident speech model ready'),
        ('STT: TL seg=', 'Speech segment'),
        ('STT: TL fe_end', 'Speech capture ended'),
        ('HTTP_CLIENT: esp_transport_read returned:0', 'HTTP read returned no data'),
        ('[G2-TERMINAL] closed:', 'Terminal closed'),
    )
    for marker, label in labels:
        if marker in line:
            ok = re.search(r'\bok=(\d)', line)
            if ok:
                label += ' succeeded' if ok[1] == '1' else ' failed'
            return {'t': timestamp, 'label': label}
    return None


def parse_sample(line, timestamp):
    """Whitelist numeric counters; never publish arbitrary serial output."""
    start = line.find('{"schema":')
    if start < 0:
        return None
    try:
        obj, _ = json.JSONDecoder().raw_decode(line[start:])
        dram, ps, caps = obj['dram'], obj['psram'], obj['heapCaps']
        fields = {'internalTotal': dram['total'], 'internalFree': dram['free'],
                  'internalMin': dram['minFree'], 'internalLargest': caps['internalLargest'],
                  'psramTotal': ps.get('total', 0), 'psramFree': ps.get('free', 0)}
        if any(type(v) is not int or v < 0 for v in fields.values()):
            return None
        if any(fields[k] > fields['internalTotal'] for k in
               ('internalFree', 'internalMin', 'internalLargest')):
            return None
        if fields['psramFree'] > fields['psramTotal']:
            return None
        return dict(t=timestamp, **fields)
    except (ValueError, KeyError, TypeError):
        return None


class Monitor:
    def __init__(self, fifo, log, output, interval):
        self.fifo, self.log, self.output, self.interval = fifo, log, output, interval
        self.samples = deque(maxlen=3600)
        self.events = deque(maxlen=300)
        self.event_file = output.with_suffix('.events.jsonl')
        for path, dest in ((output, self.samples), (self.event_file, self.events)):
            if path.exists():
                with path.open() as saved:
                    for line in saved:
                        try:
                            dest.append(json.loads(line))
                        except ValueError:
                            pass
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.status = 'Waiting for first sample'

    def snapshot(self):
        with self.lock:
            return {'samples': list(self.samples), 'status': self.status,
                    'events': list(self.events),
                    'now': time.time(), 'interval': self.interval, 'threshold': 153600}

    def run(self):
        # Append only sanitized memory counters, never raw console or credentials.
        fd = os.open(self.output, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        event_fd = os.open(self.event_file, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
        with os.fdopen(fd, 'a', buffering=1) as saved, os.fdopen(event_fd, 'a', buffering=1) as events, self.log.open(errors='replace') as log:
            log.seek(0, os.SEEK_END)
            pending, deadline, next_send, partial = False, 0, 0, ''
            while not self.stop.is_set():
                now = time.monotonic()
                chunk = log.read(65536)
                partial += chunk
                while '\n' in partial:
                    line, partial = partial.split('\n', 1)
                    event = parse_event(line, time.time())
                    if event:
                        with self.lock:
                            self.events.append(event)
                        events.write(json.dumps(event) + '\n')
                    sample = parse_sample(line, time.time())
                    if sample:
                        with self.lock:
                            self.samples.append(sample)
                            self.status = 'Receiving samples'
                        saved.write(json.dumps(sample) + '\n')
                        pending = False
                        next_send = now + self.interval
                    elif 'Login required' in line or 'Not authenticated' in line:
                        with self.lock:
                            self.status = 'Serial login required'
                partial = partial[-65536:]
                if pending and now >= deadline:
                    with self.lock:
                        self.status = 'No USB reply — readings are stale'
                    pending = False
                    next_send = now + 10  # no command backlog during USB silence
                if not pending and now >= next_send:
                    try:
                        out = os.open(self.fifo, os.O_WRONLY | os.O_NONBLOCK)
                        try:
                            os.write(out, b'memreport json\n')
                        finally:
                            os.close(out)
                        pending, deadline = True, now + 8
                    except OSError:
                        with self.lock:
                            self.status = 'Serial logger is not running'
                        next_send = now + 10
                self.stop.wait(.1)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--fifo', type=Path, required=True)
    p.add_argument('--log', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--interval', type=float, default=2)
    p.add_argument('--port', type=int, default=8766)
    args = p.parse_args()
    if args.interval < 1:
        p.error('Use an interval of at least one second')
    monitor = Monitor(args.fifo, args.log, args.output, args.interval)
    page = Path(__file__).with_suffix('.html').read_bytes()

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == '/':
                body, kind = page, 'text/html; charset=utf-8'
            elif self.path == '/data':
                body, kind = json.dumps(monitor.snapshot()).encode(), 'application/json'
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header('Content-Type', kind)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Cache-Control', 'no-store')
            self.send_header('X-Content-Type-Options', 'nosniff')
            self.send_header('Content-Security-Policy', "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; frame-ancestors 'none'")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(('127.0.0.1', args.port), Handler)
    worker = threading.Thread(target=monitor.run, daemon=True)
    worker.start()
    print(f'RAM monitor: http://127.0.0.1:{args.port}/', flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        monitor.stop.set()
        worker.join(timeout=2)
        server.server_close()


if __name__ == '__main__':
    main()
