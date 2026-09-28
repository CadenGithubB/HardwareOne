#!/usr/bin/env python3
"""Apply the shared battery changes over frozen qualified P4/S3 app copies.

No serial access. Capture current production changes against the recorded base,
then prepare/check private build copies. --refresh only updates a previously
verified battery copy; qualified originals and downloaded SDKs are untouched.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BASE = 'a95a9aa'
COMPONENT = 'components/hardwareone/'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def jpeg_module():
    spec = importlib.util.spec_from_file_location('battery_jpeg_baseline', HERE.parent / 'jpeg_portable/prepare.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def capture():
    names = subprocess.check_output(['git', 'diff', '--name-only', BASE, '--', COMPONENT], cwd=REPO).decode().splitlines()
    names += subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard', '--', COMPONENT], cwd=REPO).decode().splitlines()
    names = sorted(set(p for p in names if '/test/' not in p and Path(p).suffix in ('.h', '.cpp', '.txt')))
    if not names:
        raise ValueError('no production battery changes')
    tracked = set(subprocess.check_output(['git', 'ls-files', '--', COMPONENT], cwd=REPO).decode().splitlines())
    chunks = []
    for path in names:
        if path in tracked:
            chunks.append(subprocess.check_output(['git', 'diff', '--binary', BASE, '--', path], cwd=REPO))
        else:
            run = subprocess.run(['git', 'diff', '--no-index', '--binary', '--', '/dev/null', path], cwd=REPO, stdout=subprocess.PIPE, check=False)
            if run.returncode != 1:
                raise ValueError('new-file patch failed: ' + path)
            chunks.append(run.stdout)
    (HERE / 'integration.patch').write_bytes(b''.join(chunks))
    (HERE / 'integration-inputs.json').write_text(json.dumps({'schema': 1, 'base_commit': BASE,
        'production_sources': {p: sha(REPO / p) for p in names},
        'patch_sha256': sha(HERE / 'integration.patch')}, indent=2) + '\n')
    print(f'Captured {len(names)} production sources')


def inputs(target):
    record = json.loads((HERE / 'integration-inputs.json').read_text())
    if record['patch_sha256'] != sha(HERE / 'integration.patch'):
        raise ValueError('integration patch changed')
    for path, expected in record['production_sources'].items():
        if sha(REPO / path) != expected:
            raise ValueError('production source changed; recapture then refresh: ' + path)
    return {'integration_inputs_sha256': sha(HERE / 'integration-inputs.json'),
            'feature_profile_sha256': sha(HERE / f'features-{target}.h'),
            'radio_shims': {p: sha(HERE / p) for p in ('radio_backend.cpp', 'radio_backend.h')}}


def baseline(target):
    jpeg = jpeg_module()
    source = HERE.parent / f'jpeg_portable/private/app-{target}'
    jpeg.verify(source, target, check_worktree=False)
    _, expected, common = jpeg.baseline_definition(target)
    original = json.loads((source / 'jpeg-qualification.json').read_text())
    expected.update(original['overlay'])
    return source, expected, common


def check(target, destination, current=True):
    source, expected, common = baseline(target)
    record = json.loads((destination / 'battery-build-manifest.json').read_text())
    if record['target'] != target or record['schema'] != 1:
        raise ValueError('battery manifest target/schema mismatch')
    if record['baseline_manifest_sha256'] != sha(source / 'jpeg-qualification.json'):
        raise ValueError('baseline manifest changed')
    if current and record['inputs'] != inputs(target):
        raise ValueError('build inputs changed')
    expected.update(record['overlay'])
    common.verify_files(destination, expected, 'battery app')
    return len(expected)


def prepare(target, destination, refresh=False):
    if destination.exists() and not refresh:
        raise ValueError('destination exists; use --check or explicit --refresh')
    if refresh:
        check(target, destination, current=False)
    source, expected, common = baseline(target)
    input_record = inputs(target)
    changed = json.loads((HERE / 'integration-inputs.json').read_text())['production_sources']
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.battery-stage-', dir=destination.parent) as directory:
        stage = Path(directory)
        for relative in expected:
            out = common.safe_path(stage, relative)
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(common.safe_path(source, relative), out)
        patch = subprocess.run(['patch', '-f', '-F', '0', '-p', '1'], input=(HERE / 'integration.patch').read_bytes(), cwd=stage, capture_output=True)
        report = (patch.stdout + patch.stderr).decode()
        if patch.returncode or 'fuzz' in report:
            raise ValueError('battery patch failed exact-context application: ' + report)
        record = {'schema': 1, 'target': target, 'baseline_manifest_sha256': sha(source / 'jpeg-qualification.json'),
                  'inputs': input_record, 'overlay': {p: sha(stage / p) for p in changed}}
        (stage / 'battery-build-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
        count = check(target, stage)
        if refresh:
            old = json.loads((destination / 'battery-build-manifest.json').read_text())
            for relative in set(old['overlay']) | set(record['overlay']):
                src, dst = stage / relative, destination / relative
                if not src.is_file():
                    raise ValueError('removing overlay files requires a new copy')
                dst.parent.mkdir(parents=True, exist_ok=True)
                if not dst.exists() or sha(src) != sha(dst):
                    shutil.copy2(src, dst)
            shutil.copy2(stage / 'battery-build-manifest.json', destination / 'battery-build-manifest.json')
        else:
            shutil.copytree(stage, destination)
            if (source / 'managed_components').is_dir():
                shutil.copytree(source / 'managed_components', destination / 'managed_components')
    return count


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture', action='store_true')
    p.add_argument('--target', choices=('p4', 's3'))
    p.add_argument('--check', action='store_true')
    p.add_argument('--refresh', action='store_true')
    args = p.parse_args()
    if args.capture:
        capture()
        return
    if not args.target:
        p.error('--target required')
    destination = HERE / f'private/app-{args.target}'
    count = check(args.target, destination) if args.check else prepare(args.target, destination, args.refresh)
    print(f'Verified {count} sources: {destination}')


if __name__ == '__main__':
    main()
