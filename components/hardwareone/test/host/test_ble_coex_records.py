#!/usr/bin/env python3
"""Run the shared BLE coexistence records and the Wi-Fi state classifier.

Both headers are dependency-free (docs/WIFI_BLE_COEXISTENCE_STUDY.md, lever 1);
the phone pipe's own use of them runs in `phone_runtime`.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]

# With ENABLE_BLE_COEX_RECORDS=0 the hooks link against no-op stubs and the
# readers say the records are absent; every runtime function needs one.
STUB_MAIN = r"""
#include <cassert>
#include <cstring>
int main() {
  using namespace hw1coex;
  const uint8_t address[6] = {};
  bleCoexScanBegin(Peer::Ring, Purpose::Discovery, 160, 158, true, false, 5);
  bleCoexOnGapEvent(0, nullptr);
  bleCoexOnScanMatch(-60);
  assert(bleCoexScanMatches() == 0);
  bleCoexScanEnd(ScanOutcome::Found);
  bleCoexConnectBegin(Peer::Ring, Purpose::Saved, address);
  bleCoexConnectEnd(true, 8, 0, 100);
  bleCoexSetAccessoryWait(1500);
  bleCoexClearAccessoryWait();
  bleCoexLogSetupWait("g2 saved", 1500);
  bleCoexInstallGattcObserver();
  bleCoexRecordScan(ScanRecord{});
  bleCoexRecordConnect(ConnectRecord{});
  assert(bleCoexWifiNow().state == WifiCoex::Unknown);
  char text[128];
  assert(bleCoexScanCounters(text, sizeof(text), Peer::Phone) == std::strlen(text));
  assert(std::strstr(text, "not in this build"));
  char small[8];
  assert(bleCoexScanCounters(small, sizeof(small), Peer::Phone) == 7 && std::strlen(small) == 7);
  assert(bleCoexScanCounters(small, 0, Peer::Phone) == 0);
  std::strcpy(text, "stale");
  assert(bleCoexScanLines(text, sizeof(text), 0, Peer::Phone, 8) == 0 && text[0] == 0);
  assert(bleCoexConnectCounters(text, sizeof(text), Peer::Phone) == std::strlen(text) && std::strstr(text, "not in this build"));
  std::strcpy(text, "stale");
  assert(bleCoexConnectLines(text, sizeof(text), 0, Peer::Phone, 8) == 0 && text[0] == 0);
  assert(!std::strncmp(bleCoexCommand(""), "Error: ", 7) && std::strstr(bleCoexCommand("clear"), "not in this build"));
  return 0;
}
"""


def stub_unit() -> str:
    source = (ROOT / 'BLE_CoexRecords.cpp').read_text()
    header = (ROOT / 'BLE_CoexRecords.h').read_text()
    assert source.startswith('// BLE coexistence measurement records') and '#if ENABLE_BLE_COEX_RECORDS\n' in source
    # Every build without the records (with or without Bluetooth) gets the stubs.
    stubs = source.split('\n#else\n', 1)[1].split('#endif  // ENABLE_BLE_COEX_RECORDS', 1)[0]
    prologue = source.split('#if ENABLE_BLE_COEX_RECORDS\n', 1)[0].replace('#include "System_BuildConfig.h"\n', '')
    runtime = header.split('// ---- Runtime', 1)[1]
    function = r'^(?![ /#])[^\n(;]*\b(bleCoex\w+)\('
    declared = set(re.findall(function, runtime, re.MULTILINE))
    defined = set(re.findall(function, stubs, re.MULTILINE))
    assert declared and declared == defined, f'stubs differ from the header: {sorted(declared ^ defined)}'
    assert all(f'{name}(' in STUB_MAIN for name in declared), 'the stub check calls every runtime function'
    return prologue + stubs + STUB_MAIN


def check_flag_wiring() -> None:
    config = (ROOT / 'System_BuildConfig.h').read_text()
    editable, derived = config.split('// CHECKED-IN DEPLOYMENT PROFILE OVERRIDE', 1)
    assert re.search(r'^#define ENABLE_BLE_COEX_RECORDS 1$', editable, re.MULTILINE), 'an editable choice, on by default'
    assert re.search(r'#if !ENABLE_BLUETOOTH\n  #undef ENABLE_BLE_COEX_RECORDS\n  #define ENABLE_BLE_COEX_RECORDS 0\n#endif',
                     derived), 'off without Bluetooth'
    # The command row and its handler stay in every Bluetooth build (never #if-wrap a registry row).
    assert 'ENABLE_BLE_COEX_RECORDS' not in (ROOT / 'Bluetooth.cpp').read_text()
    wifi = (ROOT / 'System_WiFi.h').read_text()
    assert '#if ENABLE_WIFI && ENABLE_BLE_COEX_RECORDS\nWifiCoexTag wifiCoexTag();' in wifi, 'no mirror without the records'


if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    check_flag_wiring()
    with tempfile.TemporaryDirectory(prefix='hw1-ble-coex-') as tmp:
        binary=Path(tmp)/'scan'
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                 '-I',str(ROOT),str(HERE/'ble_coex_records_harness.cpp'),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
        unit,stub=Path(tmp)/'stubs.cpp',Path(tmp)/'stubs'
        unit.write_text(stub_unit())
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror','-pedantic',
                 '-I',str(ROOT),str(unit),'-o',str(stub)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(stub)],check=True)
        print('BLE records compiled out: every hook is a no-op stub; readers report the records absent')
