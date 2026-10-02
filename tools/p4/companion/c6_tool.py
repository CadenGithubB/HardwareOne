#!/usr/bin/env python3
"""Explicit backup/programming commands for the isolated P4 service firmware."""
import argparse
import hashlib
from pathlib import Path
import time
import serial


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--port', default='/dev/cu.usbmodem2101')
    p.add_argument('action', choices=['info', 'read', 'hash', 'write', 'run', 'monitor'])
    p.add_argument('--offset', type=lambda s: int(s, 0), default=0)
    p.add_argument('--size', type=lambda s: int(s, 0))
    p.add_argument('--file', type=Path)
    args = p.parse_args()
    dev = serial.Serial(port=None, baudrate=115200, timeout=1)
    dev.dtr = False
    dev.rts = False
    dev.port = args.port
    dev.open()

    def send(s):
        dev.write(s.encode() + b'\n')
        dev.flush()

    def lines(timeout=120):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            raw = dev.readline()
            if not raw:
                continue
            line = raw.decode('ascii', 'replace').strip()
            if line.startswith('ERROR'):
                raise RuntimeError(line)
            if line:
                yield line
        raise TimeoutError('P4 programmer response timeout')

    def until(prefix, timeout=120):
        for line in lines(timeout):
            print(line, flush=True)
            if line.startswith(prefix):
                return line

    time.sleep(.3)
    dev.reset_input_buffer()
    try:
        if args.action == 'monitor':
            send('monitor')
            until('DONE_MONITOR', 20)
            return
        if args.action == 'run':
            send('run')
            until('RUNNING')
            return
        send('connect')
        info = until('INFO chip=esp32c6')
        flash_size = int(info.split('flash=')[1].split()[0])
        if args.action == 'info':
            return
        size = args.size or flash_size
        if args.action == 'write':
            if args.file is None:
                p.error('write requires --file')
            data = args.file.read_bytes()
            size = len(data)
        if args.offset < 0 or size <= 0 or args.offset + size > flash_size:
            raise ValueError('Requested operation is outside the C6 flash')
        if args.action == 'read':
            if args.file is None:
                p.error('read requires --file')
            if args.file.exists():
                raise FileExistsError(args.file)
            partial = args.file.with_suffix(args.file.suffix + '.partial')
            digest = hashlib.sha256()
            count = 0
            expected_sha = None
            send(f'read {args.offset} {size}')
            with partial.open('xb') as output:
                for line in lines(600):
                    if line.startswith('DATA '):
                        _, addr, n, encoded = line.split()
                        block = bytes.fromhex(encoded)
                        if int(addr, 16) != args.offset + count or len(block) != int(n) or count + len(block) > size:
                            raise ValueError('Invalid backup stream position/length')
                        output.write(block)
                        digest.update(block)
                        count += len(block)
                        if count % 262144 == 0:
                            print(f'Backup {count}/{size}', flush=True)
                    elif line.startswith('SHA256 '):
                        expected_sha = line.split()[1]
                    elif line == 'DONE_READ':
                        break
                    else:
                        print(line, flush=True)
            if count != size or expected_sha != digest.hexdigest():
                raise ValueError('Backup length or device SHA-256 mismatch')
            partial.rename(args.file)
            print(f'BACKUP_VERIFIED bytes={count} sha256={expected_sha}', flush=True)
        elif args.action == 'hash':
            send(f'hash {args.offset} {size}')
            until('DONE_READ', 600)
        elif args.action == 'write':
            if args.offset % 4096:
                raise ValueError('Flash write offset must be sector aligned')
            print(f'WRITE bytes={size} offset={args.offset} sha256={hashlib.sha256(data).hexdigest()}', flush=True)
            send(f'write {args.offset} {size}')
            until('READY_WRITE')
            for pos in range(0, size, 1024):
                block = data[pos:pos+1024]
                send('DATA ' + block.hex())
                response = next(lines())
                if response != f'WROTE {pos + len(block)}':
                    raise ValueError(f'Unexpected write response: {response}')
                if (pos + len(block)) % 65536 == 0:
                    print(f'Written {pos + len(block)}/{size}', flush=True)
            response = until('DONE_WRITE')
            if response != 'DONE_WRITE verify=0':
                raise ValueError(response)
            send(f'hash {args.offset} {size}')
            expected = hashlib.sha256(data).hexdigest()
            actual = None
            for line in lines(600):
                print(line, flush=True)
                if line.startswith('SHA256 '):
                    actual = line.split()[1]
                elif line == 'DONE_READ':
                    break
            if actual != expected:
                raise ValueError('Post-flash SHA-256 mismatch')
            print('FLASH_READBACK_VERIFIED', flush=True)
    finally:
        dev.close()


if __name__ == '__main__':
    main()
