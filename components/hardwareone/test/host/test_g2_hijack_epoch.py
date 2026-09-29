#!/usr/bin/env python3
"""Exercise actual G2 command admission/completion against exact owner epochs."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
HERE=Path(__file__).resolve().parent

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--component',type=Path,default=HERE.parents[1]);p.add_argument('--sanitize',action='store_true')
    a=p.parse_args();c=a.component.resolve();sys.path.insert(0,str(c/'test/host'))
    from test_web_batch_handlers import extract_block
    source=(c/'G2_HijackCmd.cpp').read_text();header=(c/'G2_HijackCmd.h').read_text()
    start=header.index('bool g2SubmitHijackCommand(');prototype=header[start:header.index(';',start)+1]
    code=extract_block(source,'struct HijackCallContext')+';\n'+extract_block(source,'void g2HijackInternalCallback(')+'\n'+prototype+'\n'+extract_block(source,'bool g2SubmitHijackCommand(')
    path=HERE/'g2_hijack_epoch_harness.cpp'
    start=header.index('typedef void (*G2HijackCmdCallback)');callback=header[start:header.index(';',start)+1]
    harness=path.read_text();assert harness.count('// INSERT_PRODUCTION')==1;harness=harness.replace('// INSERT_PRODUCTION',code).replace('// INSERT_CALLBACK',callback)
    with tempfile.TemporaryDirectory(prefix='hw1-g2-epoch-') as tmp:
        unit=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';unit.write_text(harness)
        cmd=['clang++','-std=c++17','-Wall','-Wextra','-Werror',str(unit),'-o',str(exe)]
        if a.sanitize:cmd[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
        subprocess.run(cmd,check=True,timeout=120);subprocess.run([str(exe)],check=True,timeout=60)
if __name__=='__main__':main()
