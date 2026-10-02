#!/usr/bin/env python3
"""Decode the HW1LM1 parity fixtures with the production decoder.

fixtures/stt_lm/ is written by experiments/stt_train/lm/make_fixtures.py from
the host reference decoder (hw1lm.py; contract FORMAT.md): tiny.lm, raw int8
[frames x 29] logits per case, custom_words.txt and manifest.json
(format hw1lm-parity-1). stt_lm_tool (stt/stt_lm.cpp and the production greedy
CTC in stt/quartznet_frontend.cpp, unmodified) decodes every case; greedy and
LM texts must match exactly. Exits 77 (CTest skip) when no manifest exists.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
SKIP = 77
HEADER = struct.Struct('<8sIIIIIfffIfIf8s')  # FORMAT.md header table
PARAM_FIELDS = {'alpha': 6, 'beta': 7, 'unk_log10': 8, 'beam_width': 9, 'char_prune': 10, 'hotword_bonus': 12}


def f32(x):
    return struct.unpack('<f', struct.pack('<f', float(x)))[0]


def with_params(blob, params):
    """The LM with the case's decoder params in its header (SHA-256 resealed)."""
    fields = list(HEADER.unpack_from(blob, 0))
    changed = False
    for name, index in PARAM_FIELDS.items():
        if name in params:
            value = int(params[name]) if name == 'beam_width' else f32(params[name])
            changed |= fields[index] != value
            fields[index] = value
    if not changed:
        return blob, False
    data = bytearray(blob)
    HEADER.pack_into(data, 0, *fields)
    data[-32:] = hashlib.sha256(bytes(data[:-32])).digest()
    return bytes(data), True


def decode(tool, lm_path, logits_path, exponent, words_path=None):
    command = [tool, 'decode', '--lm', str(lm_path), '--logits', str(logits_path), '--exponent', str(exponent)]
    if words_path:
        command += ['--words', str(words_path)]
    run = subprocess.run(command, capture_output=True, text=True, timeout=120)
    try:
        return json.loads(run.stdout)
    except json.JSONDecodeError:
        return {'load': 'tool error', 'status': f'exit {run.returncode}: {run.stderr.strip()}', 'lm': None, 'greedy': None}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tool', required=True, help='path to the built stt_lm_tool')
    parser.add_argument('--fixtures', type=Path, default=HERE / 'fixtures' / 'stt_lm')
    args = parser.parse_args()
    manifest_path = args.fixtures / 'manifest.json'
    if not manifest_path.is_file():
        print(f'SKIP: no {manifest_path} (written by experiments/stt_train/lm/make_fixtures.py)')
        return SKIP
    manifest = json.loads(manifest_path.read_text())
    if manifest.get('format') != 'hw1lm-parity-1':
        raise SystemExit(f"unsupported fixture format {manifest.get('format')!r}")
    if 'q * 2^-exponent' not in manifest.get('dequantise', ''):
        raise SystemExit(f"unexpected dequantisation contract: {manifest.get('dequantise')!r}")
    root = args.fixtures
    blob = (root / manifest['lm']).read_bytes()
    if hashlib.sha256(blob).hexdigest() != manifest['lm_sha256'] or len(blob) != manifest['lm_bytes']:
        raise SystemExit(f"{manifest['lm']} does not match the manifest")
    failures = checked = 0
    with tempfile.TemporaryDirectory(prefix='hw1-stt-lm-parity-') as tmp:
        for case in manifest['cases']:
            name = case['name']
            logits = root / case['file']
            data = logits.read_bytes()
            problems = []
            if hashlib.sha256(data).hexdigest() != case['sha256'] or len(data) != case['frames'] * case['classes']:
                problems.append('logits file does not match the manifest')
            lm_blob, patched = with_params(blob, case.get('params') or manifest.get('params') or {})
            lm_path = root / manifest['lm']
            if patched:
                lm_path = Path(tmp) / f'{name}.lm'
                lm_path.write_bytes(lm_blob)
            # Manifest: logit = q * 2^-exponent; the decoder takes the ESP-DL
            # tensor exponent (logit = q * 2^exponent), i.e. the negation.
            exponent = -int(case['exponent'])
            words = root / case['custom_words'] if case.get('custom_words') else None
            runs = [('lm', words, case['expected_lm'])]
            if 'expected_lm_without_custom_words' in case:
                runs.append(('lm without custom words', None, case['expected_lm_without_custom_words']))
            result = None
            for label, words_path, expected in runs:
                result = decode(args.tool, lm_path, logits, exponent, words_path)
                if result['load'] != 'ok' or result['status'] != 'ok':
                    problems.append(f"{label}: load={result['load']} status={result['status']}")
                elif result['lm'] != expected:
                    problems.append(f"{label} {result['lm']!r} != {expected!r}")
                if label == 'lm':
                    if result.get('frames') != case['frames']:
                        problems.append(f"frames {result.get('frames')} != {case['frames']}")
                    if result['greedy'] != case['expected_greedy']:
                        problems.append(f"greedy {result['greedy']!r} != {case['expected_greedy']!r}")
            checked += 1
            if problems:
                failures += 1
                print(f"FAIL {name} ({case.get('source')}): " + '; '.join(problems))
            else:
                print(f"ok   {name} ({case.get('source')}): {case['frames']} frames, beam {result['beam']}, "
                      f"margin {case.get('margin')}, {result['decode_us'] / 1000:.1f} ms"
                      f"{' (params patched)' if patched else ''}: {result['lm']!r}")
    print(f'stt_lm parity: {checked - failures}/{checked} cases match')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
