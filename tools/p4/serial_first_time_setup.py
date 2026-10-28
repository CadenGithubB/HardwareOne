#!/usr/bin/env python3
"""Drive first-time setup over the USB console after an erase + flash.

    ~/.espressif/python_env/idf5.5_py3.14_env/bin/python3 tools/p4/serial_first_time_setup.py \
        /dev/cu.usbmodem1301 p4_seriallog.txt <username> <password> 300

Resets the board on open, answers Basic setup / archetype 1 / username /
password / Light theme as each prompt appears, then keeps capturing for the
given number of seconds after "FIRST-TIME SETUP COMPLETE". Every console line
is appended to the log file with a host timestamp; the credentials are never
written to it.
"""
import datetime, sys, time
import serial

port, out_path, user, pw, tail = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], float(sys.argv[5])
steps = [
    (b"Serial: Enter 1..", "1"),                    # setup mode: Basic
    (b"WHAT WILL YOU USE THIS DEVICE FOR?", "1"),   # archetype 1 (default)
    (b"Admin Username:", user),
    (b"Admin Password:", pw),
    (b"WEB UI THEME", "1"),                         # Light
    (b"FIRST-TIME SETUP COMPLETE", None),
]

s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.1
s.dtr = False; s.rts = True; s.open(); time.sleep(0.1); s.rts = False   # reset on open

def stamp():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

out = open(out_path, "ab")
out.write(f"==== setup-drive start {datetime.datetime.now():%Y-%m-%d %H:%M:%S} (reset) ====\n".encode())
acc = b""      # accumulated bytes since the last matched trigger
linebuf = b""
step = 0
done_at = None
started = time.monotonic()
deadline = started + 600
try:
    while time.monotonic() < deadline:
        chunk = s.read(4096)
        if chunk:
            linebuf += chunk
            while b"\n" in linebuf:
                line, linebuf = linebuf.split(b"\n", 1)
                out.write(f"[{stamp()}] ".encode() + line.rstrip(b"\r") + b"\n")
            out.flush()
            acc += chunk
            if step < len(steps):
                trig, resp = steps[step]
                i = acc.find(trig)
                if i >= 0:
                    acc = acc[i + len(trig):]
                    if resp is None:
                        done_at = time.monotonic()
                        print(f"{stamp()} step {step}: matched {trig!r} (complete)", flush=True)
                    else:
                        time.sleep(0.5)                       # let the prompt finish printing
                        s.write(resp.encode() + b"\n"); s.flush()
                        shown = "***" if trig == b"Admin Password:" or trig == b"Admin Username:" else resp
                        out.write(f"[{stamp()}] >>> sent: {shown}\n".encode())
                        print(f"{stamp()} step {step}: matched {trig!r}, sent {shown}", flush=True)
                    step += 1
        if done_at is not None and time.monotonic() - done_at >= tail:
            break
finally:
    if linebuf:
        out.write(f"[{stamp()}] ".encode() + linebuf + b"\n")
    out.write(f"==== setup-drive end (step {step}/{len(steps)}) ====\n".encode())
    out.close()
    s.rts = False; s.dtr = False; s.close()
print(f"finished at step {step}/{len(steps)}", flush=True)
