#!/usr/bin/env python3
"""The BLE console mirror buffers only while a session could receive it.

bleMirrorRecipientCount() (Bluetooth.cpp) counts the sessions sendBLEResponse
would reach: it runs the same collector, bleCollectMirrorSessions. Both are
spliced from Bluetooth.cpp into a harness with a four-slot connection table: a
connected peer that never logged in, a guest session, a slot whose session
epoch was cleared and an inactive slot are not recipients; logged-in non-guest
sessions are. Source checks: the debug drain in System_Debug.cpp gates its BLE
buffer on that count (with only guest sessions logged in it used to fill and
drop the buffer for nothing), and `outble` reports it. --source-dir points the
suite at another copy of the component (the pre-fix tree has no count).
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
typedef uint32_t TransportSessionEpoch;
struct String {
  std::string s;
  String() = default;
  String(const char* p) : s(p) {}
  size_t length() const { return s.size(); }
};
constexpr int BLE_MAX_CONNECTIONS = 4;
struct BLEConnection {
  uint16_t connId = 0;
  bool active = false, authed = false, mirrorEligible = false;
  String user;
};
struct BLEState { BLEConnection connections[BLE_MAX_CONNECTIONS]; };
static BLEState sState;
static BLEState* gBLEState = &sState;
static std::atomic<TransportSessionEpoch> sBleSessionEpoch[BLE_MAX_CONNECTIONS];
static bool gLifecycleAvailable = true;
struct BleLifecycleGuard { explicit operator bool() const { return gLifecycleAvailable; } };

@FENCE@;
@COLLECT@
@COUNT@

static void slot(int i, uint16_t conn, bool authed, bool eligible, const char* user, TransportSessionEpoch epoch) {
  BLEConnection& c = sState.connections[i];
  c = BLEConnection{};
  c.active = true; c.connId = conn; c.authed = authed; c.mirrorEligible = eligible; c.user = String(user);
  sBleSessionEpoch[i].store(epoch);
}
int main() {
  int bad = 0;
  auto expect = [&](bool ok, const char* what) { if (!ok) { std::printf("FAIL %s\n", what); ++bad; } };
  for (int i = 0; i < BLE_MAX_CONNECTIONS; ++i) { sState.connections[i] = BLEConnection{}; sBleSessionEpoch[i].store(0); }
  expect(bleMirrorRecipientCount() == 0, "an empty table has recipients");
  slot(0, 0, false, false, "", 5);          // connected, never logged in
  slot(1, 1, true, false, "visitor", 6);    // guest session
  expect(bleMirrorRecipientCount() == 0, "a guest or a peer that never logged in counts as a mirror recipient");
  slot(2, 2, true, true, "owner", 0);       // logged out: epoch cleared
  expect(bleMirrorRecipientCount() == 0, "a slot without a live session epoch counts as a recipient");
  slot(3, 3, true, true, "admin", 9);
  expect(bleMirrorRecipientCount() == 1, "a logged-in non-guest session is not counted");
  slot(0, 0, true, true, "owner", 7);       // conn 0 is a live link
  expect(bleMirrorRecipientCount() == 2, "connection 0 is not counted");
  sState.connections[3].active = false;
  expect(bleMirrorRecipientCount() == 1, "a disconnected slot is counted");
  gLifecycleAvailable = false;
  expect(bleMirrorRecipientCount() == 0, "the count read the table without the lifecycle lock");
  return bad ? 1 : 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('clang++') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--source-dir', '--component', dest='source_dir', type=Path, default=COMPONENT)
    args = parser.parse_args()
    bluetooth = (args.source_dir / 'Bluetooth.cpp').read_text()
    debug = (args.source_dir / 'System_Debug.cpp').read_text()
    failures = []

    count_marker = 'int bleMirrorRecipientCount()'
    if count_marker not in bluetooth:
        failures.append('Bluetooth.cpp has no bleMirrorRecipientCount(): the console mirror cannot ask '
                        'whether a flush would reach anybody')
    else:
        count = extract_block(bluetooth, count_marker)
        if 'bleCollectMirrorSessions(' not in count:
            failures.append('bleMirrorRecipientCount() does not use the collector sendBLEResponse uses')
        harness = (HARNESS.replace('@FENCE@', extract_block(bluetooth, 'struct BleSessionFence'))
                   .replace('@COLLECT@', extract_block(bluetooth, 'static int bleCollectMirrorSessions('))
                   .replace('@COUNT@', count))
        with tempfile.TemporaryDirectory(prefix='hw1-ble-mirror-recipients-') as tmp:
            unit = Path(tmp) / 'harness.cpp'
            exe = Path(tmp) / 'test'
            unit.write_text(harness)
            command = [args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                       str(unit), '-o', str(exe)]
            if args.sanitize:
                command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            subprocess.run(command, check=True)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            if run.returncode:
                failures.append('recipient count: ' + run.stdout.strip())

    gate = '(msg->routing & MSG_ROUTE_BLE) && (gOutputFlags & MSG_ROUTE_BLE)'
    drain = debug[debug.index(gate):]
    condition = drain[:drain.index('{')]
    if 'bleMirrorRecipientCount() > 0' not in condition:
        failures.append('the debug drain buffers BLE output without asking whether a session could '
                        f'receive it: {" ".join(condition.split())}')
    outble = extract_block(debug, 'const char* cmd_outble(const String& argsInput)')
    if 'bleMirrorRecipientCount()' not in outble or 'mirror recipients' not in outble:
        failures.append('`outble` status does not report the mirror recipient count')

    for message in failures:
        print('FAIL', message, file=sys.stderr)
    if failures:
        sys.exit(1)
    print('BLE console mirror: the recipient count matches sendBLEResponse (logged-in non-guest sessions '
          'only); the drain buffers only with a recipient; outble reports the count')


if __name__ == '__main__':
    main()
