import serial, time, re, sys
s = serial.Serial(); s.port='/dev/cu.usbmodem2101'; s.baudrate=115200; s.timeout=0.05; s.dtr=False; s.rts=False; s.open()
log = open(sys.argv[1], 'wb'); buf = ''; mark = 0; sent = []; t_end = time.time() + 150; quiet_since = time.time()
def send(x, why):
    s.write((x + '\r').encode()); sent.append(why); print(f'   -> sent {why}'); time.sleep(1.0)
while time.time() < t_end:
    d = s.read(4096)
    if d:
        log.write(d); buf += d.decode('utf-8', 'replace'); quiet_since = time.time(); continue
    if time.time() - quiet_since < 0.8: continue   # act only once output settles
    new = buf[mark:]; mark = len(buf)
    if not new.strip(): continue
    tail = [l.strip() for l in new.splitlines() if l.strip() and not l.strip().startswith(('I (', 'W (', 'E ('))][-8:]
    if 'FIRST-TIME' in buf or 'SETUP' in buf: print('P4 screen:', ' | '.join(tail)[:400])
    low = new.lower()
    if 'enter 1 or 2' in low and 'mode' not in sent: send('1', 'mode')
    elif 'username' in low and 'user' not in sent: send('a', 'user')
    elif 'password' in low and 'pass' in sent and 'confirm' not in sent and ('confirm' in low or 'again' in low): send('a', 'confirm')
    elif 'password' in low and 'pass' not in sent: send('a', 'pass')
    elif "'n' next" in low: send('n', 'next-page')
    if re.search(r'(?i)setup complete', buf) and 'pass' in sent: time.sleep(4); break
s.close(); print('sent:', sent)
