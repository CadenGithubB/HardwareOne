#!/usr/bin/env python3
"""Compile the complete G2 transcription page against bounded UI/command fakes.

Production JSON parsing, ownership checks, navigation, polling, command construction,
receipt handling and UTF-8 recent-tail policy execute unchanged. No BLE or FS I/O.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component',type=Path,default=HERE.parents[1])
    p.add_argument('--source-dir',type=Path)
    p.add_argument('--sanitize',action='store_true')
    a=p.parse_args(); component=a.component.resolve()
    def read(name):
        overlay=a.source_dir/name if a.source_dir else None
        return (overlay if overlay and overlay.exists() else component/name).read_text()
    source=read('G2_Page_Transcription.cpp')
    source=re.sub(r'^#include.*\n','',source,flags=re.M)
    hp=a.source_dir/'g2_transcription_harness.cpp' if a.source_dir else HERE/'g2_transcription_harness.cpp'
    harness=hp.read_text()
    for tag,value in [('// INSERT_PRODUCTION',source),('// INSERT_POLICY',read('Transcription_UI_Policy.h').replace('#pragma once','')),
                      ('// INSERT_PAGER',read('System_TextPager.h').replace('#pragma once',''))]:
        assert harness.count(tag)==1
        harness=harness.replace(tag,value)
    with tempfile.TemporaryDirectory(prefix='hw1-g2-transcription-') as tmp:
        unit=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';unit.write_text(harness)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I',str(component.parent/'hardwareone_libs/ArduinoJson/src'),str(unit),'-o',str(exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True,timeout=120);subprocess.run([str(exe)],check=True,timeout=60)
if __name__=='__main__':main()
