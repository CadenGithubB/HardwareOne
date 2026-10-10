#!/usr/bin/env python3
"""Execute the Sensors page's shipped empty-grid decision with a small DOM boundary.

This checks that the Microphone navigation target survives on a build with no
detected I2C sensors or camera. It does not exercise layout or audio hardware.
--baseline replays the same expectations against a saved Sensors header.
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

CHECKS = r"""
const assert = require('node:assert/strict');
const vm = require('node:vm');
const source = JSON.parse(require('node:fs').readFileSync(0, 'utf8'));
for (const [mic, sensor, expectedEmpty] of [
  [true, null, false], [false, null, true], [true, 'imu', false],
  [false, 'imu', false], [false, 'camera', false],
]) {
  const keys = ['imu', 'thermal', 'tof', 'gamepad', 'anoencoder', 'gps',
                'servo', 'fmradio', 'camera', 'rtc', 'presence'];
  const has = Object.fromEntries(keys.map(key => [key, key === sensor]));
  const compiled = {...has};
  const markup = mic ? '<div id="mic-card">Microphone</div>' : '<div>Sensor</div>';
  const grid = {innerHTML: markup};
  const micCard = mic ? {} : null;
  vm.runInNewContext(source, {
    has, compiled, grid, hw: {$: id => id === 'mic-card' ? micCard : null},
    console: {log() {}},
  });
  if (expectedEmpty) {
    assert.match(grid.innerHTML, /No sensors available/);
  } else {
    assert.equal(grid.innerHTML, markup,
                 `Sensors must preserve the target (microphone=${mic}, sensor=${sensor})`);
  }
}
console.log('Sensors: microphone-only target and empty/available sensor grids passed');
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--node', default=os.environ.get('NODE') or shutil.which('node'))
    parser.add_argument('--baseline', type=Path)
    args = parser.parse_args()
    if not args.node:
        raise SystemExit('Pass --node /path/to/node for JavaScript behavior tests')
    header = args.baseline or COMPONENT / 'WebPage_Sensors.h'
    if header.is_dir():
        header /= 'WebPage_Sensors.h'
    # Execute the actual streamed chunk; do not duplicate its any/empty policy.
    match = re.search(r'httpd_resp_send_chunk\(req, ("var any=(?:[^"\\]|\\.)*")',
                      header.read_text())
    if not match:
        raise AssertionError('missing production Sensors empty-grid chunk')
    source = json.loads(match.group(1))
    subprocess.run([args.node, '-e', CHECKS], input=json.dumps(source), text=True,
                   check=True, timeout=30)


if __name__ == '__main__':
    main()
