#!/usr/bin/env python3
"""Read-only check of the exact Arduino BLE seams needed by the phone pipe."""
import argparse
import hashlib
import json
from pathlib import Path

HERE=Path(__file__).resolve().parent
EXPECTED_FILES={f'libraries/BLE/src/{name}' for name in ('BLEDevice.cpp','BLEDevice.h','BLESecurity.cpp','BLESecurity.h','BLEClient.cpp')}

def check(source, assets=HERE):
    manifest=json.loads((assets/'manifest.json').read_text())
    if manifest.get('schema') != 1 or set(manifest.get('files',{})) != EXPECTED_FILES:
        raise ValueError('unsupported phone-pipe SDK manifest')
    digest=lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    if digest(assets/'arduino.patch') != manifest['patch_sha256']:
        raise ValueError('phone-pipe patch checksum mismatch')
    missing=[]
    for name, hashes in manifest['files'].items():
        relative=Path(name)
        if relative.is_absolute() or '..' in relative.parts:
            raise ValueError('invalid SDK manifest path')
        path=source/relative
        if not path.is_file() or path.is_symlink():
            missing.append(name+': missing or symlink')
        else:
            current=digest(path)
            if current != hashes['after']:
                missing.append(name+(': apply the reviewed patch' if current == hashes['before'] else ': unrecognized changes; review required'))
    if missing:
        raise ValueError('\n'.join(missing))

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,default=HERE.parents[1]/'components/arduino')
    args=parser.parse_args()
    try:
        check(args.source)
    except (OSError,ValueError,KeyError) as exc:
        parser.exit(1,str(exc)+'\n')
    print('Phone-pipe Arduino BLE hooks match the reviewed patch.')
