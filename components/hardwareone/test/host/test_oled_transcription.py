#!/usr/bin/env python3
"""Compile the shipping OLED Transcription mode and async bridge with host fakes."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
COMPONENT=HERE.parents[1]
def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    source=(COMPONENT/'OLED_Mode_Transcription.cpp').read_text()
    source=re.sub(r'^#include.*\n','',source,flags=re.M)
    harness=(HERE/'oled_transcription_harness.cpp').read_text()
    harness=harness.replace('// INSERT_PRODUCTION',source)
    harness=harness.replace('// INSERT_POLICY',(COMPONENT/'Transcription_UI_Policy.h').read_text().replace('#pragma once',''))
    bridge=extract_block((COMPONENT/'OLED_Utils.cpp').read_text(),'bool submitOLEDCommandForSession(')
    harness=harness.replace('// INSERT_BRIDGE',bridge)
    with tempfile.TemporaryDirectory(prefix='hw1-oled-transcription-') as tmp:
        unit=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';unit.write_text(harness)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-const-variable','-I',str(COMPONENT.parent/'hardwareone_libs/ArduinoJson/src'),str(unit),'-o',str(exe)]
        if args.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(cmd,check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
