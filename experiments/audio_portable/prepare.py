#!/usr/bin/env python3
"""Build-only audio overlay on verified, preserved portable-camera app copies.

Capture the production diff against 05614eb only after edits are frozen. Prepare
separate private app copies; no serial access, flashing or SDK mutation occurs.
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
CAMERA = HERE.parent / 'camera_portable'
BASE = '05614eb5900e461633ad951f7358b522894e313b'
COMPONENT = 'components/hardwareone/'
DEP_FILE = COMPONENT + 'idf_component.yml'
MANIFEST = 'audio-build-manifest.json'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def camera_module():
    spec = importlib.util.spec_from_file_location('audio_camera_baseline', CAMERA / 'prepare.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def production_changes():
    names = subprocess.check_output(
        ['git', 'diff', '--name-only', BASE, '--', COMPONENT], cwd=REPO).decode().splitlines()
    names += subprocess.check_output(
        ['git', 'ls-files', '--others', '--exclude-standard', '--', COMPONENT],
        cwd=REPO).decode().splitlines()
    return sorted(set(p for p in names if '/test/' not in p and
                      Path(p).suffix in ('.h', '.cpp', '.txt')))


def capture():
    names = production_changes()
    if not names:
        raise ValueError('No production audio changes to capture')
    # This milestone reuses the SDK's existing I2S driver. A component dependency
    # change needs an explicitly reviewed dependency workflow, not silent drift.
    dependency = subprocess.check_output(['git', 'show', BASE + ':' + DEP_FILE], cwd=REPO)
    if dependency != (REPO / DEP_FILE).read_bytes():
        raise ValueError('Component dependency source changed from the qualified base')
    tracked = set(subprocess.check_output(
        ['git', 'ls-files', '--', COMPONENT], cwd=REPO).decode().splitlines())
    chunks = []
    for path in names:
        if not (REPO / path).is_file():
            raise ValueError('Deleted production files require a new baseline: ' + path)
        if path in tracked:
            chunks.append(subprocess.check_output(
                ['git', 'diff', '--unified=2', '--binary', BASE, '--', path], cwd=REPO))
        else:
            result = subprocess.run(['git', 'diff', '--no-index', '--binary', '--',
                                     '/dev/null', path], cwd=REPO, capture_output=True)
            if result.returncode != 1:
                raise ValueError('New-file patch failed: ' + path)
            chunks.append(result.stdout)
    (HERE / 'integration.patch').write_bytes(b''.join(chunks))
    record = {
        'schema': 1, 'base_commit': BASE,
        'production_sources': {p: sha(REPO / p) for p in names},
        'dependency_source_sha256': sha(REPO / DEP_FILE),
        'patch_sha256': sha(HERE / 'integration.patch'),
    }
    (HERE / 'integration-inputs.json').write_text(json.dumps(record, indent=2) + '\n')
    print('Captured', len(names), 'production sources')


def sdkconfig_paths(target):
    roles = HERE.parent / 'p4_ble_roles'
    paths = [roles / 'sdkconfig.connectivity.defaults']
    if target == 'p4':
        paths.append(roles / 'sdkconfig.p4.bluetooth.defaults')
    paths.append(CAMERA / ('sdkconfig.' + target + '.camera.defaults'))
    return paths


def inputs(target):
    data = json.loads((HERE / 'integration-inputs.json').read_text())
    if data['schema'] != 1 or data['base_commit'] != BASE:
        raise ValueError('Captured source schema/base mismatch')
    if data['patch_sha256'] != sha(HERE / 'integration.patch'):
        raise ValueError('Integration patch changed')
    if data['dependency_source_sha256'] != sha(REPO / DEP_FILE):
        raise ValueError('Component dependency source changed')
    if sorted(data['production_sources']) != production_changes():
        raise ValueError('Production file set changed; capture and refresh')
    for path, digest in data['production_sources'].items():
        if sha(REPO / path) != digest:
            raise ValueError('Source changed; capture and refresh: ' + path)
    external = [HERE / 'radio_backend.cpp', HERE / 'radio_backend.h',
                CAMERA / 'radio_backend.cpp', CAMERA / 'radio_backend.h',
                HERE.parent / 'jpeg_portable/radio_backend.cpp',
                HERE.parent / 'jpeg_portable/radio_backend.h',
                HERE.parent / 'p4_ble_roles/radio_backend.cpp',
                HERE.parent / 'p4_ble_roles/radio_backend.h',
                HERE.parent / 'p4_espnow/bridge/esp_now_hosted_host.c',
                HERE.parent / 'p4_espnow/bridge/esp_now_hosted_host.h',
                HERE.parent / 'p4_espnow/bridge/esp_now_hosted_rpc.h']
    return {
        'integration_inputs_sha256': sha(HERE / 'integration-inputs.json'),
        'prepare_sha256': sha(HERE / 'prepare.py'),
        'build_sha256': sha(HERE / 'build.sh'),
        'profile_sha256': sha(HERE / ('features-' + target + '.h')),
        'inherited_profile_sha256': sha(CAMERA / ('features-' + target + '.h')),
        'sdkconfig_sha256': {str(p.relative_to(REPO)): sha(p) for p in sdkconfig_paths(target)},
        'radio_shims': {str(p.relative_to(REPO)): sha(p) for p in external},
    }


def baseline(target):
    camera = camera_module()
    source = CAMERA / ('private/app-' + target)
    # Current production files are deliberately changing for audio. Validate the
    # qualified camera copy against its sealed file manifest, not today's tree.
    camera.check(target, source, current=False)
    _, expected, common = camera.baseline(target)
    expected.update(json.loads((source / 'camera-build-manifest.json').read_text())['overlay'])
    return source, expected, common


def check(target, destination, current=True):
    source, expected, common = baseline(target)
    data = json.loads((destination / MANIFEST).read_text())
    if data['schema'] != 1 or data['target'] != target or data['base_commit'] != BASE:
        raise ValueError('Audio manifest target/schema/base mismatch')
    if data['baseline_manifest_sha256'] != sha(source / 'camera-build-manifest.json'):
        raise ValueError('Camera baseline manifest changed')
    if current and data['inputs'] != inputs(target):
        raise ValueError('Audio build inputs changed')
    expected.update(data['overlay'])
    common.verify_files(destination, expected, 'audio application')
    return len(expected)


def prepare(target, destination, refresh=False):
    if destination.is_symlink():
        raise ValueError('Refusing a symlink destination')
    if destination.exists() and not refresh:
        raise ValueError('Destination exists; use --check or explicit --refresh')
    if refresh:
        check(target, destination, current=False)
    source, expected, common = baseline(target)
    input_record = inputs(target)
    changes = json.loads((HERE / 'integration-inputs.json').read_text())['production_sources']
    destination.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with tempfile.TemporaryDirectory(prefix='.audio-stage-', dir=destination.parent) as directory:
        stage = Path(directory)
        for relative in expected:
            output = common.safe_path(stage, relative)
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(common.safe_path(source, relative), output)
        result = subprocess.run(['patch', '-f', '-F', '0', '-p', '1'],
                                input=(HERE / 'integration.patch').read_bytes(),
                                cwd=stage, capture_output=True)
        report = (result.stdout + result.stderr).decode()
        if result.returncode or 'fuzz' in report:
            raise ValueError('Exact-context audio patch failed: ' + report)
        data = {
            'schema': 1, 'target': target, 'base_commit': BASE,
            'baseline_manifest_sha256': sha(source / 'camera-build-manifest.json'),
            'inputs': input_record,
            'overlay': {p: sha(common.safe_path(stage, p)) for p in changes},
        }
        (stage / MANIFEST).write_text(json.dumps(data, indent=2) + '\n')
        count = check(target, stage)
        if refresh:
            old = json.loads((destination / MANIFEST).read_text())
            for relative in set(old['overlay']) | set(data['overlay']):
                src = common.safe_path(stage, relative)
                dst = common.safe_path(destination, relative)
                if not src.is_file():
                    raise ValueError('Removing an overlay needs a new copy')
                dst.parent.mkdir(parents=True, exist_ok=True)
                if not dst.exists() or sha(src) != sha(dst):
                    shutil.copy2(src, dst)
            shutil.copy2(stage / MANIFEST, destination / MANIFEST)
        else:
            # No symlinks/hardlinks: future Component Manager actions must never
            # alter the camera qualification copy or downloaded SDK directories.
            shutil.copytree(stage, destination)
            shutil.copytree(source / 'managed_components', destination / 'managed_components')
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument('--capture', action='store_true')
    actions.add_argument('--check', action='store_true')
    actions.add_argument('--refresh', action='store_true')
    parser.add_argument('--target', choices=('p4', 's3'))
    args = parser.parse_args()
    if args.capture:
        return capture()
    if not args.target:
        parser.error('--target required')
    destination = HERE / ('private/app-' + args.target)
    count = (check(args.target, destination) if args.check else
             prepare(args.target, destination, args.refresh))
    print('Verified', count, 'sources:', destination)


if __name__ == '__main__':
    main()
