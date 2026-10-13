#!/usr/bin/env python3
"""Run the ESP-NOW millis() deadlines across the 32-bit wrap with an injectable clock.

Unit 1 compiles the production prev-key unwrap, rekey apply and prev-key sweep
from System_ESPNow_Sessions.cpp; unit 2 compiles the production sensor
subscription apply with its lease-expiry owner (System_ESPNow_Sensors.cpp) and
the session pre-warm backoff owners (System_ESPNow.cpp). The clock is placed on
both sides of the rollover: a 5 s previous-key window, a leased sensor
subscription and a 60 s pre-warm cooldown must neither outlive the wrap by weeks
nor lapse at once, a deadline landing on zero must never be stored as "none",
and oversized leases are capped at INT32_MAX ms. Source contracts confirm the
broadcaster task and the heartbeat handler call those owners instead of
comparing absolute times. System_ESPNow_Deadline.h is pasted when the overlay or
the component has it. No radio or hardware is accessed.
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
NEW_OWNERS = (('System_ESPNow_Sensors.cpp', 'static bool sensorLeaseLapsed('),
              ('System_ESPNow.cpp', 'static bool sessionPrewarmInCooldown('),
              ('System_ESPNow.cpp', 'static bool sessionPrewarmNoteAttempt('))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    p.add_argument('--source-dir', type=Path,
                   help='directory whose System_ESPNow_Sessions.{cpp,h}, System_ESPNow_Sensors.{cpp,h}, '
                        'System_ESPNow.cpp, System_ESPNow_Wire.h and System_ESPNow_Deadline.h replace '
                        'the component copies')
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    sys.path.insert(0, str(a.component / 'test/host'))
    from test_web_batch_handlers import extract_block

    def locate(name):
        overlay = a.source_dir / name if a.source_dir else None
        return overlay if overlay and overlay.exists() else a.component / name

    def read(name):
        return locate(name).read_text()

    def read_optional(name):
        path = locate(name)
        return path.read_text() if path.exists() else ''

    def line(source, pattern, what):
        m = re.search(pattern, source, re.M)
        if not m:
            raise SystemExit(f'test_espnow_deadline_wrap: {what} not found in the production source')
        return m.group(0)

    def one(source, marker, what):
        if source.count(marker) != 1:
            raise SystemExit(f'test_espnow_deadline_wrap: {what} has {source.count(marker)} copies of {marker!r}')
        return extract_block(source, marker)

    deadline = read_optional('System_ESPNow_Deadline.h')
    failures = []

    def run_unit(unit, harness, label):
        with tempfile.TemporaryDirectory(prefix=f'hw1-espnow-deadline-{unit}-') as tmp:
            src, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
            src.write_text(harness)
            cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pedantic', f'-DA16_UNIT={unit}',
                   str(src), '-o', str(exe)]
            if a.sanitize:
                cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
            subprocess.run(cmd, check=True, timeout=120)
            if subprocess.run([str(exe)], timeout=60).returncode:
                failures.append(f'unit {unit} ({label}) reported failures')

    def fill(tags):
        harness = (HERE / 'espnow_deadline_wrap_harness.cpp').read_text()
        for tag, value in tags:
            assert harness.count(tag) == 1, tag
            harness = harness.replace(tag, value)
        return harness

    # ---- Unit 1: previous-key window (compiles on every tree) -------------------
    sessions = read('System_ESPNow_Sessions.cpp')
    sessions_h = read('System_ESPNow_Sessions.h')
    wire = read('System_ESPNow_Wire.h')
    session_types = '\n'.join([
        one(sessions_h, 'enum SessionStateLifecycle', 'System_ESPNow_Sessions.h') + ';',
        line(sessions_h, r'^constexpr uint32_t kRekeyPrevKeysWindowMs = .*;', 'kRekeyPrevKeysWindowMs'),
        one(sessions_h, 'struct SessionState', 'System_ESPNow_Sessions.h') + ';',
    ])
    internal = '\n\n'.join([
        line(sessions, r'^constexpr uint8_t kSessionSlots = .*;', 'kSessionSlots'),
        line(sessions, r'^SessionState\* gSessions = .*;', 'gSessions'),
        line(sessions, r'^constexpr size_t kAeadTagBytes = .*;', 'kAeadTagBytes'),
        one(sessions, 'void buildNonce(', 'System_ESPNow_Sessions.cpp'),
        one(sessions, 'bool replayCheckAndInsert(', 'System_ESPNow_Sessions.cpp'),
    ])
    production = '\n\n'.join(one(sessions, marker, 'System_ESPNow_Sessions.cpp') for marker in
                             ('bool sessionUnwrapFrame(', 'void sessionApplyRekeyedKeys(',
                              'void sessionRekeyPrevKeysSweep('))
    run_unit(1, fill([('// INSERT_DEADLINE', deadline),
                      ('// INSERT_WIRE', one(wire, 'struct __attribute__((packed)) EspNowV4Header',
                                             'System_ESPNow_Wire.h') + ';'),
                      ('// INSERT_SESSION_TYPES', session_types),
                      ('// INSERT_SESSION_INTERNAL', internal),
                      ('// INSERT_SESSION_PRODUCTION', production),
                      ('// INSERT_SENSOR_TYPES', ''), ('// INSERT_LEASE_STATE', ''),
                      ('// INSERT_SENSOR_PRODUCTION', ''), ('// INSERT_PREWARM_PRODUCTION', '')]),
             'prev-key window')

    # ---- Source contracts: the periodic callers use the owners ------------------
    sensors = read('System_ESPNow_Sensors.cpp')
    espnow = read('System_ESPNow.cpp')
    task = one(sensors, 'static void sensorBroadcasterTask(', 'System_ESPNow_Sensors.cpp')
    if 'sensorLeaseLapsed(i, (uint32_t)now)' not in task or '>= gSensorLeaseExpiresAt' in task:
        failures.append('sensorBroadcasterTask compares the lease deadline with an absolute >= '
                        'instead of calling sensorLeaseLapsed(i, (uint32_t)now)')
    heartbeat = one(espnow, 'static void v4h_heartbeat(', 'System_ESPNow.cpp')
    if ('sessionPrewarmInCooldown(*pw, now)' not in heartbeat or
            'sessionPrewarmNoteAttempt(pw, now)' not in heartbeat or
            '>= pw->cooldownUntilMs' in heartbeat or '+ kSessionPrewarmCooldownMs' in heartbeat):
        failures.append('v4h_heartbeat compares/arms the pre-warm cooldown with absolute millis() arithmetic '
                        'instead of calling sessionPrewarmInCooldown/sessionPrewarmNoteAttempt')

    # ---- Unit 2: sensor leases and pre-warm cooldown (needs the A16 owners) ------
    sources = {'System_ESPNow_Sensors.cpp': sensors, 'System_ESPNow.cpp': espnow}
    missing = [f'{name} lacks the wrap-safe owner {marker}): the A16 fix is absent'
               for name, marker in NEW_OWNERS if sources[name].count(marker) != 1]
    if missing:
        failures.extend(missing)
    else:
        sensors_h = read('System_ESPNow_Sensors.h')
        sensor_types = '\n'.join(one(sensors_h, marker, 'System_ESPNow_Sensors.h') + ';'
                                 for marker in ('enum RemoteSensorType', 'enum SensorReqMode'))
        lease_state = line(sensors, r'^static uint32_t gSensorLeaseExpiresAt\[REMOTE_SENSOR_MAX\] = .*;',
                           'gSensorLeaseExpiresAt')
        sensor_production = '\n\n'.join(one(sensors, marker, 'System_ESPNow_Sensors.cpp') for marker in
                                        ('static bool sensorLeaseLapsed(', 'void espnowApplySensorSubscription('))
        constants = re.findall(r'^static constexpr \w+\s+kSessionPrewarm\w+\s*=\s*.*;', espnow, re.M)
        if len(constants) != 2:
            raise SystemExit('test_espnow_deadline_wrap: kSessionPrewarm* constants not found in System_ESPNow.cpp')
        prewarm = '\n'.join(constants + [
            one(espnow, 'struct SessionPrewarm', 'System_ESPNow.cpp') + ';',
            line(espnow, r'^EXT_RAM_BSS_ATTR static SessionPrewarm gSessionPrewarm\[6\];', 'gSessionPrewarm')])
        prewarm += '\n\n' + '\n\n'.join(one(espnow, marker, 'System_ESPNow.cpp') for marker in
                                        ('static SessionPrewarm* sessionPrewarmSlot(',
                                         'static void sessionPrewarmReset(',
                                         'static bool sessionPrewarmInCooldown(',
                                         'static bool sessionPrewarmNoteAttempt('))
        run_unit(2, fill([('// INSERT_DEADLINE', deadline),
                          ('// INSERT_WIRE', ''), ('// INSERT_SESSION_TYPES', ''),
                          ('// INSERT_SESSION_INTERNAL', ''), ('// INSERT_SESSION_PRODUCTION', ''),
                          ('// INSERT_SENSOR_TYPES', sensor_types),
                          ('// INSERT_LEASE_STATE', lease_state),
                          ('// INSERT_SENSOR_PRODUCTION', sensor_production),
                          ('// INSERT_PREWARM_PRODUCTION', prewarm)]),
                 'sensor leases, pre-warm cooldown')

    for failure in failures:
        print('FAIL', failure, file=sys.stderr)
    if failures:
        print(f'test_espnow_deadline_wrap: {len(failures)} failure(s)', file=sys.stderr)
        sys.exit(1)
    print('test_espnow_deadline_wrap: all cases passed')


if __name__ == '__main__':
    main()
