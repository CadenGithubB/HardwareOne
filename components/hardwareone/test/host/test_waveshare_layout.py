#!/usr/bin/env python3
"""Check real landscape rasterization, pixel bounds and rotated touch actions."""
from pathlib import Path
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent

def main():
    with tempfile.TemporaryDirectory(prefix='hw1-waveshare-layout-') as tmp:
        binary=Path(tmp)/'checks'
        subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-I',str(HERE.parents[1]),
                        str(HERE/'waveshare_layout_harness.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
    print('Waveshare: both layouts, every rasterized pixel, controls and gesture transition passed')
if __name__=='__main__': main()
