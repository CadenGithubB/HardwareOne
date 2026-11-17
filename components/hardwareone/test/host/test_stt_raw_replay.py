#!/usr/bin/env python3
"""Build and check stt_raw_replay (the `sttraw` host replay CLI) on synthetic audio.

The signal is room-like noise (RMS ~4) with one ordinary-level phrase (RMS ~60) and one
quiet phrase (RMS ~15), so it straddles the production floors (speech 45, silence 16):
- default settings admit the ordinary phrase only, as on the device;
- lowering both floors also admits the quiet phrase;
- the result does not depend on the push chunk size (the device pushes 1024-sample reads;
  --trace pushes 10 ms frames);
- the sidecar's dropped-sample count is reported and the rate is checked;
- the level-robust onset (--decision-pre 97, the device default) admits a quiet phrase of
  hiss-like speech that the raw level alone misses, its noise-control flags reach the
  segmenter, and the result is again chunk-independent; the default stays raw level only.
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


def synth_hf():
    """Room noise with a quiet phrase whose energy sits near Nyquist (RMS ~30, below the
    45-count onset floor): the raw level misses it, its pre-emphasised level does not."""
    rng = random.Random(20261115)
    out = [rng.gauss(0.0, 4.0) for _ in range(int(3.0 * RATE))]
    n = int(2.0 * RATE)
    for i in range(n):
        env = 0.5 + 0.5 * math.sin(math.pi * 4 * i / RATE) ** 2
        out.append(30.0 / math.sqrt(0.625) * env * rng.gauss(0.0, 1.0) * (1 if i % 2 else -1)
                   + rng.gauss(0.0, 4.0))
    out.extend(rng.gauss(0.0, 4.0) for _ in range(int(3.0 * RATE)))
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
        # Raw level only unless asked (stored raw-only arms reproduce); like the device's
        # sidecar, the onset's gain and noise controls read 0 while it is off.
        assert cfg['detector'] == 'v3.1' and cfg['decision_pre'] == 0, cfg
        assert (cfg['decision_gain_pct'], cfg['decision_modulation_ratio_pct'],
                cfg['decision_pre_hold_requires_raw']) == (0, 0, 0), cfg
        ignored, _ = run(exe, pcm, '--decision-mod', '300', '--decision-hold', '1')
        assert spans(ignored) == spans(base) and ignored['config']['decision_modulation_ratio_pct'] == 0
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

        hf = tmp / 'raw_hf.pcm'
        hf.write_bytes(synth_hf())
        raw_only, _ = run(exe, hf)
        assert not inside(raw_only, 4.0), spans(raw_only)
        onset, _ = run(exe, hf, '--decision-pre', '97')
        assert onset['config']['decision_pre'] == 97 and onset['config']['decision_gain_pct'] == 150
        assert inside(onset, 4.0) and not inside(onset, 7.5), spans(onset)
        for chunk in ('1', '777', '4096'):
            other, _ = run(exe, hf, '--decision-pre', '97', '--chunk', chunk)
            assert spans(other) == spans(onset), (chunk, spans(other), spans(onset))
        gated, _ = run(exe, hf, '--decision-pre', '97', '--decision-mod', '300', '--decision-hold', '1',
                       '--decision-gain', '200')
        g = gated['config']
        assert (g['decision_modulation_ratio_pct'], g['decision_pre_hold_requires_raw'], g['decision_gain_pct']) == (300, 1, 200), g
        for chunk in ('1', '4096'):
            other, _ = run(exe, hf, '--decision-pre', '97', '--decision-mod', '300', '--decision-hold', '1',
                           '--decision-gain', '200', '--chunk', chunk)
            assert spans(other) == spans(gated), (chunk, spans(other), spans(gated))
        for flag, value in (('--decision-mod', '50'), ('--decision-mod', '1001'), ('--decision-pre', '101'),
                            ('--decision-gain', '24'), ('--decision-hold', '2')):
            bad = subprocess.run([str(exe), str(hf), flag, value], capture_output=True, text=True)
            assert bad.returncode == 2 and flag in bad.stderr, (flag, value, bad.stderr)

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
