#!/usr/bin/env python3
"""Build and check stt_raw_replay (the `sttraw` host replay CLI) on synthetic audio.

The signal is room-like noise (RMS ~4) with one ordinary-level phrase (RMS ~60) and one
quiet phrase (RMS ~15), so it straddles the production floors (speech 45, silence 16):
- default settings admit the ordinary phrase only, as on the device;
- lowering both floors also admits the quiet phrase;
- the result does not depend on the push chunk size (the device pushes 1024-sample reads;
  --trace pushes 10 ms frames);
- the sidecar's dropped-sample count is reported and the rate is checked.
No board or recordings are used.
"""
import argparse
import json
import math
import random
import struct
import subprocess
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RATE = 16000


def synth():
    rng = random.Random(20261107)
    out = []

    def noise(seconds, sigma=4.0):
        out.extend(rng.gauss(0.0, sigma) for _ in range(int(seconds * RATE)))

    def phrase(seconds, rms):
        # A 180 Hz tone with a 4 Hz syllable envelope, on top of the room noise.
        n = int(seconds * RATE)
        amp = rms * math.sqrt(2) / math.sqrt(0.375)  # mean of sin^4 envelope is 0.375
        for i in range(n):
            env = math.sin(math.pi * 4 * i / RATE) ** 2
            out.append(amp * env * math.sin(2 * math.pi * 180 * i / RATE) + rng.gauss(0.0, 4.0))

    noise(3.0)
    phrase(2.0, 60)   # ordinary: about -55 dBFS
    noise(3.0)
    phrase(2.0, 15)   # quiet: about -67 dBFS
    noise(3.0)
    return b''.join(struct.pack('<h', max(-32768, min(32767, int(round(v))))) for v in out)


def run(exe, pcm, *extra):
    result = subprocess.run([str(exe), str(pcm), *extra], check=True, capture_output=True, text=True, timeout=60)
    return json.loads(result.stdout), result.stderr


def spans(report):
    return [(s['start'], s['end'], s['end_kind']) for s in report['segments']]


def inside(report, seconds):
    return any(s['start'] <= seconds * RATE < s['end'] for s in report['segments'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='clang++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='hw1-stt-raw-replay-') as tmp:
        tmp = Path(tmp)
        exe = tmp / 'replay'
        cmd = [args.cxx, '-std=c++17', '-O1', '-Wall', '-Wextra', '-Werror', '-pedantic',
               '-I' + str(HERE.parents[1]), str(HERE / 'stt_raw_replay.cpp'), '-o', str(exe)]
        if args.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(cmd, check=True, timeout=180)
        pcm = tmp / 'raw_test.pcm'
        pcm.write_bytes(synth())

        base, _ = run(exe, pcm)
        cfg = base['config']
        assert (cfg['speech_floor'], cfg['silence_floor'], cfg['noise_percentile']) == (45, 16, 25), cfg
        assert (cfg['min_segment'], cfg['short_close'], cfg['max_segment']) == (64000, 16000, 160000), cfg
        assert cfg['blip_samples'] == 480 and cfg['pre_roll'] == 4800, cfg
        assert inside(base, 4.0), 'the ordinary phrase must be admitted at production floors'
        assert not inside(base, 9.0), 'the quiet phrase must stay below production floors'

        for chunk in ('160', '1', '777', '4096'):
            other, _ = run(exe, pcm, '--chunk', chunk)
            assert spans(other) == spans(base), (chunk, spans(other), spans(base))
        traced, _ = run(exe, pcm, '--trace', str(tmp / 'frames.csv'))
        assert spans(traced) == spans(base)
        rows = (tmp / 'frames.csv').read_text().splitlines()
        assert rows[0] == 'sample,rms,noise,threshold,open' and len(rows) - 1 == len(pcm.read_bytes()) // 2 // 160

        low, _ = run(exe, pcm, '--speech-floor', '10', '--silence-floor', '5')
        assert inside(low, 4.0) and inside(low, 9.0), spans(low)

        cuts = tmp / 'cuts'
        cuts.mkdir()
        run(exe, pcm, '--cut-dir', str(cuts))
        raw = pcm.read_bytes()
        for n, s in enumerate(base['segments'], 1):
            assert (cuts / f'seg_{n}.pcm').read_bytes() == raw[2 * s['start']:2 * s['end']]

        sidecar = tmp / 'raw_test.json'
        sidecar.write_text('{"format":"s16le","channels":1,"rate":16000,"samples_dropped":1024,"gaps":[[5000,1024]]}\n')
        gapped, warning = run(exe, pcm, '--sidecar', str(sidecar))
        assert gapped['sidecar_dropped'] == 1024 and 'dropped 1024' in warning
        sidecar.write_text('{"rate":48000,"samples_dropped":0}\n')
        bad = subprocess.run([str(exe), str(pcm), '--sidecar', str(sidecar)], capture_output=True, text=True)
        assert bad.returncode == 2 and 'rate' in bad.stderr
        bad = subprocess.run([str(exe), str(pcm), '--speech-floor', '10', '--silence-floor', '20'],
                             capture_output=True, text=True)
        assert bad.returncode == 2 and 'silence floor' in bad.stderr
    print('stt_raw_replay: OK')


if __name__ == '__main__':
    main()
