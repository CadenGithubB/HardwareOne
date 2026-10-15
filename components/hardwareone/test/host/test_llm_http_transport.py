#!/usr/bin/env python3
"""Exercise the complete production selector/BIO against deterministic SDK boundaries.

This checks routing, lifetime and cancellation, not cryptographic correctness;
the certificate matrix uses the actual linked mbedTLS separately.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--sanitize',action='store_true');p.add_argument('--source-root',type=Path,default=ROOT);args=p.parse_args()
    source=re.sub(r'^#include.*\n','',(args.source_root/'System_LLMHttpTransport.cpp').read_text(),flags=re.M)
    unit=(HERE/'llm_http_transport_harness.cpp').read_text().replace('// INSERT_PRODUCTION',source)
    with tempfile.TemporaryDirectory(prefix='hw1-http-transport-') as tmp:
        cpp=Path(tmp)/'transport.cpp';binary=Path(tmp)/'transport';cpp.write_text(unit)
        command=[os.environ.get('CXX','clang++'),'-std=c++17','-Wall','-Wextra','-Werror',
                 '-Wno-unused-function','-Wno-unused-parameter','-I',str(ROOT),str(cpp),'-o',str(binary)]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
