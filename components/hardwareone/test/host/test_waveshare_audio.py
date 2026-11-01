#!/usr/bin/env python3
"""Exercise the shipping Waveshare audio backends with fake I2C/I2S boundaries."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sanitize',action='store_true')
    args=p.parse_args()
    unit=(HERE/'waveshare_audio_harness.cpp').read_text()
    for marker,name in [('HEADER','HAL_Audio_Waveshare.h'),('MIC','HAL_Audio_Waveshare.cpp'),('SPEAKER','HAL_Speaker_Waveshare.cpp')]:
        source=re.sub(r'^#(?:include|pragma once).*\n','',(COMPONENT/name).read_text(),flags=re.M)
        unit=unit.replace('// INSERT_'+marker,source)
    with tempfile.TemporaryDirectory(prefix='hw1-waveshare-audio-') as tmp:
        cpp=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';cpp.write_text(unit)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(exe)]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        for enabled in (0,1):
            subprocess.run(cmd+[f'-DENABLE_MICROPHONE_SENSOR={enabled}'],check=True)
            subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
