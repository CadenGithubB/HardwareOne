#!/usr/bin/env python3
"""Levels, noise floor, spectrum and segment coverage of an `sttraw` recording.

Input is raw_<id>.pcm (s16le mono 16 kHz) as saved by the firmware's `sttraw on`; the values
are exactly what the STT segmenter received, in its units (raw int16 RMS per 10 ms frame,
DC removed per frame like the segmenter).

  analyze_raw.py raw.pcm [--from S] [--to S] [--spectrum] [--tones N]
  analyze_raw.py raw.pcm --replay replay.json --speech intervals.csv

levels    frame-RMS percentiles in raw units and dBFS (0 dBFS = 32768), DC offset, clipping.
          For a sealed-port or quiet-room capture, P25/P50 is the floor the detector tracks.
spectrum  Welch power spectrum (needs numpy) with the strongest narrow peaks, to tell an
          electrical floor (tones at display/camera rates and harmonics) from broadband noise.
coverage  with a stt_raw_replay JSON and a CSV of reference speech intervals (start_s,end_s
          per line, file seconds): the share of reference speech time outside every segment,
          and how many intervals have no segment at all.
Standard library only, except --spectrum.
"""
from __future__ import annotations

import argparse
import array
import json
import math
import sys
from pathlib import Path

RATE = 16000
FRAME = 160  # the segmenter's 10 ms analysis frame


def load(path: Path, start_s: float, end_s: float | None) -> array.array:
    pcm = array.array('h')
    data = path.read_bytes()
    if len(data) % 2:
        sys.exit('raw file has an odd byte count')
    pcm.frombytes(data)
    if sys.byteorder != 'little':
        pcm.byteswap()
    a = int(start_s * RATE)
    b = len(pcm) if end_s is None else min(len(pcm), int(end_s * RATE))
    return pcm[a:b]


def dbfs(rms: float) -> float:
    return 20 * math.log10(rms / 32768) if rms > 0 else float('-inf')


def frame_rms(pcm: array.array) -> list[int]:
    """Integer floor(sqrt(variance)) per full 10 ms frame, as stt_segmenter.h computes it."""
    out = []
    for i in range(0, len(pcm) - FRAME + 1, FRAME):
        f = pcm[i:i + FRAME]
        s = sum(f)
        sq = sum(v * v for v in f)
        centered = sq * FRAME - s * s
        out.append(math.isqrt(centered // (FRAME * FRAME)))
    return out


def levels(pcm: array.array) -> dict:
    frames = sorted(frame_rms(pcm))
    if not frames:
        sys.exit('recording is shorter than one 10 ms frame')
    pick = lambda p: frames[min(len(frames) - 1, int(len(frames) * p / 100))]  # noqa: E731
    n = len(pcm)
    mean = sum(pcm) / n
    total = math.sqrt(max(0.0, sum(v * v for v in pcm) / n - mean * mean))
    clipped = sum(1 for v in pcm if v in (-32768, 32767))
    pct = {f'p{p}': pick(p) for p in (1, 5, 25, 50, 75, 95, 99)}
    return {
        'seconds': round(n / RATE, 2),
        'dc_offset': round(mean, 2),
        'rms': round(total, 2), 'rms_dbfs': round(dbfs(total), 1),
        'frame_rms': pct,
        'frame_rms_dbfs': {k: round(dbfs(v), 1) for k, v in pct.items()},
        'clipped_samples': clipped,
        'production_floors': {'speech_45_dbfs': round(dbfs(45), 1), 'silence_16_dbfs': round(dbfs(16), 1)},
    }


def spectrum(pcm: array.array, tones: int) -> dict:
    try:
        import numpy as np
    except ImportError:
        sys.exit('--spectrum needs numpy (pip install numpy); levels and coverage do not')
    x = np.frombuffer(pcm.tobytes(), dtype='<i2').astype(np.float64) / 32768.0
    n = 4096
    if x.size < n:
        sys.exit('--spectrum needs at least 0.26 s of audio')
    win = np.hanning(n)
    hop = n // 2
    acc = np.zeros(n // 2 + 1)
    count = 0
    for i in range(0, x.size - n + 1, hop):
        seg = x[i:i + n]
        acc += np.abs(np.fft.rfft((seg - seg.mean()) * win)) ** 2
        count += 1
    psd = acc / count / (RATE * (win ** 2).sum())  # one-sided density, full scale = 1.0
    psd[1:-1] *= 2
    freqs = np.fft.rfftfreq(n, 1 / RATE)
    db = 10 * np.log10(psd + 1e-30)
    bands = {}
    for lo, hi in ((20, 100), (100, 300), (300, 1000), (1000, 3000), (3000, 8000)):
        m = (freqs >= lo) & (freqs < hi)
        bands[f'{lo}-{hi}Hz_dbfs'] = round(10 * math.log10(psd[m].sum() * (freqs[1] - freqs[0]) + 1e-30), 1)
    # Narrow peaks: bins well above the median of their +-50 Hz neighbourhood.
    k = max(1, int(50 / (freqs[1] - freqs[0])))
    peaks = []
    for i in range(k, len(db) - k):
        if db[i] == db[i - 2:i + 3].max():
            excess = db[i] - float(np.median(db[i - k:i + k + 1]))
            if excess >= 6:
                peaks.append((excess, float(freqs[i]), float(db[i])))
    peaks.sort(reverse=True)
    return {
        'resolution_hz': round(float(freqs[1] - freqs[0]), 2),
        'band_power': bands,
        'tones': [{'hz': round(f, 1), 'db_per_hz': round(d, 1), 'above_local_median_db': round(e, 1)}
                  for e, f, d in peaks[:tones]],
    }


def coverage(replay_path: Path, speech_path: Path) -> dict:
    replay = json.loads(replay_path.read_text())
    segs = [(s['start'] / RATE, s['end'] / RATE) for s in replay['segments']]
    intervals = []
    for line in speech_path.read_text().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        a, b = (float(v) for v in line.split(',')[:2])
        if b > a:
            intervals.append((a, b))
    total = inside = 0.0
    untouched = 0
    for a, b in intervals:
        got = sum(max(0.0, min(b, e) - max(a, s)) for s, e in segs)
        total += b - a
        inside += min(got, b - a)
        if got == 0:
            untouched += 1
    return {
        'config': replay.get('config'),
        'reference_intervals': len(intervals),
        'reference_speech_s': round(total, 2),
        'outside_every_segment_s': round(total - inside, 2),
        'outside_share': round((total - inside) / total, 4) if total else None,
        'intervals_with_no_segment': untouched,
        'segments': len(segs),
    }


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('pcm', type=Path)
    p.add_argument('--from', dest='start', type=float, default=0.0, help='start, seconds into the file')
    p.add_argument('--to', dest='end', type=float, help='end, seconds into the file')
    p.add_argument('--spectrum', action='store_true')
    p.add_argument('--tones', type=int, default=12)
    p.add_argument('--replay', type=Path, help='stt_raw_replay JSON for --speech coverage')
    p.add_argument('--speech', type=Path, help='CSV of reference speech intervals, file seconds')
    a = p.parse_args()
    if bool(a.replay) != bool(a.speech):
        p.error('--replay and --speech go together')
    out = {'file': str(a.pcm)}
    if a.replay:
        out['coverage'] = coverage(a.replay, a.speech)
    else:
        pcm = load(a.pcm, a.start, a.end)
        out['window_s'] = [a.start, a.end]
        out['levels'] = levels(pcm)
        if a.spectrum:
            out['spectrum'] = spectrum(pcm, a.tones)
    print(json.dumps(out, indent=1))


if __name__ == '__main__':
    main()
