#!/usr/bin/env python3
"""Compile the production packed-model validator with ASan/UBSan; optional real bundles."""
import argparse
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('bundles', nargs='*', type=Path)
a = p.parse_args()
here = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='hw1-srpack-') as tmp:
    exe = str(Path(tmp) / 'test')
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(here.parents[1]), str(here/'espsr_model_pack_harness.cpp'),
                    '-o', exe], check=True)
    subprocess.run([exe, *(str(b.resolve()) for b in a.bundles)], check=True)
