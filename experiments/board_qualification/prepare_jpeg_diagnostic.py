#!/usr/bin/env python3
"""Copy a verified JPEG full app and apply a qualification-only diagnostic.
No hardware access; no baseline or SDK mutation. Existing copies are checked,
never overwritten. Original S3 camera/Sense-off and P4 IO profiles are retained.
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
JPEG = HERE.parent / 'jpeg_portable'
PATCH_FILES = ('components/hardwareone/G2_Glasses.cpp', 'components/hardwareone/CMakeLists.txt')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def module():
    spec = importlib.util.spec_from_file_location('qualified_jpeg_prepare', JPEG / 'prepare.py')
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def inputs(target):
    profile = HERE.parent / ('p4_io/features.h' if target == 'p4' else 'p4_ble_roles/features.h')
    files = (HERE / 'jpeg-diagnostic.patch', HERE / 'radio_backend.cpp', HERE / 'radio_backend.h', profile)
    return {str(p.relative_to(REPO)): sha(p) for p in files}


def check(target, destination):
    jpeg = module()
    source = JPEG / f'private/app-{target}'
    jpeg.verify(source, target)
    manifest = json.loads((destination / 'jpeg-diagnostic-manifest.json').read_text())
    if manifest['target'] != target or manifest['schema'] != 1:
        raise ValueError('diagnostic target/schema changed')
    if manifest['inputs'] != inputs(target):
        raise ValueError('diagnostic patch/profile/shim inputs changed')
    if manifest['jpeg_qualification_sha256'] != sha(source / 'jpeg-qualification.json'):
        raise ValueError('qualified JPEG source changed')
    _, expected, common = jpeg.baseline_definition(target)
    original = json.loads((source / 'jpeg-qualification.json').read_text())
    expected.update(original['overlay'])
    expected.update(manifest['overlay'])
    common.verify_files(destination, expected, 'full-app diagnostic source')
    return len(expected)


def prepare(target, destination):
    if destination.exists() or destination.is_symlink():
        raise ValueError('destination exists; use --check or a new --destination')
    jpeg = module()
    source = JPEG / f'private/app-{target}'
    jpeg.verify(source, target)
    _, expected, common = jpeg.baseline_definition(target)
    original = json.loads((source / 'jpeg-qualification.json').read_text())
    expected.update(original['overlay'])
    patch = (HERE / 'jpeg-diagnostic.patch').read_bytes()
    destination.parent.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix='.jpeg-diagnostic-', dir=destination.parent))
    try:
        for relative in expected:
            src = common.safe_path(source, relative)
            dst = common.safe_path(stage, relative)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        if (source / 'managed_components').is_dir():
            shutil.copytree(source / 'managed_components', stage / 'managed_components')
        applied = subprocess.run(['patch', '-f', '-F', '0', '-p', '1'], input=patch, cwd=stage, capture_output=True)
        report = (applied.stdout + applied.stderr).decode()
        if applied.returncode or 'offset' in report or 'fuzz' in report:
            raise ValueError('diagnostic patch did not apply exactly: ' + report)
        record = {'schema': 1, 'target': target,
                  'jpeg_qualification_sha256': sha(source / 'jpeg-qualification.json'),
                  'inputs': inputs(target), 'overlay': {p: sha(stage / p) for p in PATCH_FILES},
                  'camera_enabled': False, 'sense_enabled': False}
        (stage / 'jpeg-diagnostic-manifest.json').write_text(json.dumps(record, indent=2) + '\n')
        count = check(target, stage)
        stage.rename(destination)
        return count
    finally:
        if stage.exists():
            shutil.rmtree(stage)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('p4', 's3'), required=True)
    parser.add_argument('--destination', type=Path)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    destination = args.destination or HERE / f'private/app-jpeg-{args.target}'
    count = check(args.target, destination) if args.check else prepare(args.target, destination)
    print(f'{"Verified" if args.check else "Prepared"} {count} sources: {destination}')


if __name__ == '__main__':
    main()
