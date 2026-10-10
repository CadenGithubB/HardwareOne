#!/usr/bin/env python3
"""Exercise production FM snapshots, RDS, JSON/OLED, seek and command outcomes.

Only RTOS, I2C/radio and display boundaries are substituted. Optional --baseline
compares ordinary JSON/display/RDS output to the saved pre-migration source.
"""
from __future__ import annotations
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(component: Path, baseline: bool) -> str:
    owner = (component / 'i2csensor_rda5807.cpp').read_text()
    header = (component / 'i2csensor_rda5807.h').read_text()
    types = extract_block(header, 'struct FMRadioCache') + ';\n'
    types += extract_block((COMPONENT / 'System_Mutex.h').read_text(), 'struct SensorCacheGuard') + ';\n'
    mutex = (COMPONENT / 'System_Mutex.cpp').read_text()
    functions = [extract_block(mutex, 'SensorCacheGuard::SensorCacheGuard('),
                 extract_block(mutex, 'SensorCacheGuard::~SensorCacheGuard('),
                 extract_block((COMPONENT / 'System_I2C.cpp').read_text(), 'int sensorEnvelopeBegin(')]
    if baseline:
        state = 'FMRadioCache gFmRadioCache;\n#define DATA gFmRadioCache\n#define MUTEX gFmRadioCache.mutex\n'
    else:
        names = ['sFmRadioCache', 'sFmRadioCacheMux']
        state = '\n'.join(re.search(r'(?m)^static[^\n]*\b' + name + r'\b[^\n]*;', owner)[0] for name in names)
        state += '\n' + extract_block(owner, 'struct FmRadioCacheGuard') + ';\n'
        state += '\n#define DATA sFmRadioCache\n'
        state += re.search(r'bool fmRadioCacheSnapshot\([^;]+;', header)[0] + '\n'
        functions += [extract_block(owner, marker) for marker in [
            'bool fmRadioCacheSnapshot(', 'static void fmRadioSetMuted(', 'static void fmRadioInvalidate(']]
        require('SemaphoreHandle_t' not in extract_block(header, 'struct FMRadioCache'), 'FM snapshot exposes mutex')
    functions += [extract_block(owner, marker) for marker in [
        'static void rdsScrub(', 'static void RDS_ServiceNameCallback(', 'static void RDS_TextCallback(',
        'int fmRadioBuildDataJSON(']]
    functions.append(extract_block((component / 'i2csensor_rda5807_oled.h').read_text(), 'static void displayFmRadio()'))
    if not baseline:
        functions += [extract_block(owner, marker) for marker in [
            'void updateFMRadio() ', 'const char* cmd_fmradio_tune(', 'const char* cmd_fmradio_seek(',
            'const char* cmd_fmradio_volume(', 'const char* cmd_fmradio_mute(', 'const char* cmd_fmradio_unmute(']]
    return ((HERE / 'fm_radio_snapshot_harness.cpp').read_text()
            .replace('// INSERT_TYPES', types).replace('// INSERT_STATE', state)
            .replace('// INSERT_FUNCTIONS', '\n\n'.join(functions)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), 'a C++17 host compiler is required')
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter']
    if args.sanitize: flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g']
    with tempfile.TemporaryDirectory(prefix='hw1-fm-snapshot-') as directory:
        work = Path(directory)
        def run(name: str, code: str, extra: list[str]) -> str:
            path = work / (name + '.cpp'); path.write_text(code)
            subprocess.run([args.cxx, *flags, *extra, str(path), '-o', str(work / name)], check=True, timeout=60)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)
        output = run('fm', generated(COMPONENT, False), ['-DTEST_BASELINE=0'])
        expected = json.loads((HERE / 'fixtures/fm_radio_snapshot_expected.json').read_text())
        require(output == ''.join(expected), 'FM JSON, RDS or OLED output changed')
        for line in output.splitlines():
            if line.startswith('{'): json.loads(line)
        if args.baseline:
            old = run('baseline', generated(args.baseline, True), ['-DTEST_BASELINE=1'])
            require(output == old, 'FM output differs from actual baseline')
            print('FM baseline: JSON, RDS sanitization and normal OLED output match')
        shutil.copyfile(COMPONENT / 'i2csensor_rda5807.h', work / 'i2csensor_rda5807.h')
        (work / 'Arduino.h').write_text('#include <cstddef>\n#include <cstdint>\nclass String;\n')
        (work / 'System_BuildConfig.h').write_text('#define ENABLE_FM_RADIO 0\n')
        (work / 'freertos').mkdir()
        (work / 'freertos/FreeRTOS.h').write_text('#pragma once\nusing TaskHandle_t=void*;\n')
        (work / 'freertos/task.h').write_text('#include "FreeRTOS.h"\n')
        run('disabled', '''#include <cassert>
#include "i2csensor_rda5807.h"
int main() {
 FMRadioCache out; out.dataValid=true; out.seekInProgress=true; out.stationName[0]='X';
 assert(!fmRadioCacheSnapshot(out) && !out.dataValid && !out.seekInProgress && !out.stationName[0]);
 assert(out.frequency==10390 && out.volume==6 && out.stereo);
 assert(!fmRadioCacheSnapshot(out,50));
 char buf[8]="old"; assert(fmRadioBuildDataJSON(buf,sizeof(buf))==0);
 assert(fmRadioBuildDataJSON(nullptr,0)==0 && fmRadioBuildDataJSON(nullptr,9)==0);
}
''', ['-I', str(work)])
    print('FM snapshot: bounded copies, seek interleavings, invalid/busy/issued commands, RDS and disabled API passed')


if __name__ == '__main__':
    main()
