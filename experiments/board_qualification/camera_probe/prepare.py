#!/usr/bin/env python3
"""Prepare a private camera-only project; never access hardware or overwrite captures."""
import argparse
import shutil
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('target', choices=['p4', 's3'])
args = parser.parse_args()
probe = Path(__file__).resolve().parent
source = probe / args.target
project = probe.parent / 'private' / f'camera-repro-{args.target}'
project.mkdir(parents=True, exist_ok=True)
# These are the complete source inputs; build/cache/config outputs stay private.
for relative in ('CMakeLists.txt', 'main/CMakeLists.txt', 'main/probe.c',
                 'main/idf_component.yml', 'sdkconfig.defaults', 'dependencies.lock'):
    src = source / relative
    if not src.is_file():
        raise SystemExit(f'Missing locked probe source: {src}')
    dest = project / relative
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dest)
print(project)
