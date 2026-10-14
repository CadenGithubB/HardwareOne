#!/usr/bin/env python3
"""Update the three Hardware One host modules; preserve private setup and npm."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import tempfile

MODULES = ('gateway.mjs', 'provider.mjs', 'device-api.mjs')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def update(target, source, check=False):
    target = target.expanduser().resolve(strict=True)
    extension = target / 'official/dist/hardwareone'
    provenance = target / 'provenance.json'
    if extension.resolve(strict=True) != extension or provenance.is_symlink():
        raise ValueError('Unexpected installation links; nothing changed')
    metadata = json.loads(provenance.read_text())
    if metadata.get('officialVersion') != '0.10.5':
        raise ValueError('Expected the Hardware One installation of Even Terminal 0.10.5')
    hashes = metadata.get('extensionSha256', {})
    changes = {}
    for name in MODULES:
        destination = extension / name
        if destination.is_symlink() or not destination.is_file():
            raise ValueError('Unexpected installed module: ' + name)
        old, new = destination.read_bytes(), (source / name).read_bytes()
        if digest(old) != hashes.get(name):
            raise ValueError('Installed module differs from its recorded version: ' + name)
        if not new or len(new) > 256 * 1024:
            raise ValueError('Invalid update module: ' + name)
        if old != new:
            changes[destination] = (old, new)
        hashes[name] = digest(new)
    if not changes:
        print('Hardware One host modules are already current; no files changed.')
        return
    if check:
        print('Update validated for ' + ', '.join(p.name for p in changes) + '; no files changed.')
        return
    metadata['extensionSha256'] = hashes
    changes[provenance] = (provenance.read_bytes(), (json.dumps(metadata, indent=2) + '\n').encode())
    # Validate everything before creating a backup or replacing any module.
    backup = Path(tempfile.mkdtemp(prefix='host-update-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ-'), dir=target))
    for path, (old, _) in changes.items():
        (backup / path.name).write_bytes(old)
    replaced = []
    try:
        with tempfile.TemporaryDirectory(prefix='.host-update-', dir=target) as tmp:
            for path, (_, new) in changes.items():
                staged = Path(tmp) / path.name
                staged.write_bytes(new)
            for path in changes:
                os.replace(Path(tmp) / path.name, path)
                replaced.append(path)
    except Exception:
        for path in reversed(replaced):
            path.write_bytes(changes[path][0])
        raise
    print('Updated Hardware One host modules. Backup: ' + str(backup))
    print('Credentials, identity, relay, dependencies and private configuration are unchanged.')
    print('Restart the Terminal host: node "$HOME/.local/share/hardwareone-even-terminal/start.mjs"')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--directory', type=Path, default=Path.home() / '.local/share/hardwareone-even-terminal')
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    os.umask(0o077)
    try:
        update(args.directory, Path(__file__).resolve().parent, args.check)
    except (OSError, ValueError, TypeError) as error:
        parser.exit(1, 'Host update failed: ' + str(error) + '\n')


if __name__ == '__main__':
    main()
