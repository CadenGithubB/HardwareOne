#!/usr/bin/env python3
"""Capture the USB-Serial-JTAG console to a file with host timestamps.

    ~/.espressif/python_env/idf5.5_py3.14_env/bin/python3 tools/p4/serial_capture.py \
        --port /dev/cu.usbmodem1301 --out p4_seriallog.txt [--reset] [--seconds 300]

By default the port is opened WITHOUT resetting the chip (DTR raised before
RTS, then dropped, so the USB-Serial-JTAG never sees RTS-only, which is its
reset condition on macOS). --reset holds RTS for 100 ms on open instead, so the
capture starts at the boot ROM banner. Each captured line is prefixed with the
host wall-clock time; the raw bytes are kept as-is after that prefix.
Ctrl-C (or --seconds) stops the capture and releases the port.
"""
import argparse
import datetime
import sys
import time

import serial


def open_port(port, baud, reset):
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    s.timeout = 0.1
    if reset:
        s.dtr = False
        s.rts = True        # (DTR=0, RTS=1) = chip held in reset while opening
        s.open()
        time.sleep(0.1)
        s.rts = False       # release: boots from the ROM banner
    else:
        s.dtr = True
        s.rts = False       # pyserial applies DTR then RTS -> (1,0), never (0,1)
        s.open()
        time.sleep(0.05)
        s.dtr = False
    return s


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbmodem1301")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True, help="log file (appended)")
    ap.add_argument("--reset", action="store_true", help="reset the chip on open to capture the boot")
    ap.add_argument("--seconds", type=float, default=0, help="stop after this many seconds (0 = until Ctrl-C)")
    ap.add_argument("--quiet", action="store_true", help="do not echo lines to stdout")
    args = ap.parse_args()

    s = open_port(args.port, args.baud, args.reset)
    started = time.monotonic()
    buf = b""
    lines = 0
    with open(args.out, "ab") as out:
        stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        out.write(f"==== capture start {stamp} port={args.port} reset={args.reset} ====\n".encode())
        try:
            while True:
                chunk = s.read(4096)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                        out.write(f"[{now}] ".encode() + line.rstrip(b"\r") + b"\n")
                        lines += 1
                        if not args.quiet:
                            sys.stdout.buffer.write(line.rstrip(b"\r") + b"\n")
                            sys.stdout.flush()
                    out.flush()
                if args.seconds and time.monotonic() - started >= args.seconds:
                    break
        except KeyboardInterrupt:
            pass
        finally:
            if buf:
                now = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
                out.write(f"[{now}] ".encode() + buf + b"\n")
            out.write(f"==== capture end ({lines} lines) ====\n".encode())
            s.rts = False
            s.dtr = False
            s.close()
    print(f"captured {lines} lines -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
