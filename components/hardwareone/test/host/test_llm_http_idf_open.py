#!/usr/bin/env python3
"""Run pinned IDF open/header/body-write code through the production selector."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from test_web_batch_handlers import extract_block
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--idf',type=Path,required=True);p.add_argument('--sanitize',action='store_true');args=p.parse_args()
    adapter=re.sub(r'^#include.*\n','',(ROOT/'System_LLMHttpTransport.cpp').read_text(),flags=re.M)
    base=(HERE/'llm_http_transport_harness.cpp').read_text().split('int main(){',1)[0].replace('// INSERT_PRODUCTION',adapter)
    sdk=(args.idf/'components/esp_http_client/esp_http_client.c').read_text()
    # A forward declaration precedes request_send; select definitions only.
    sdk=re.sub(r'^esp_err_t esp_http_client_request_send[^\n]*;\n', '', sdk, flags=re.M)
    functions='\n'.join(extract_block(sdk,k) for k in ('static int http_client_prepare_first_line(',
        'int esp_http_client_write(', 'esp_err_t esp_http_client_close(',
        'esp_err_t esp_http_client_request_send(', 'esp_err_t esp_http_client_open('))
    unit=base+(HERE/'llm_http_idf_open_harness.cpp').read_text().replace('// INSERT_SDK',functions)
    with tempfile.TemporaryDirectory(prefix='hw1-http-idf-open-') as tmp:
        cpp=Path(tmp)/'open.cpp';binary=Path(tmp)/'open';cpp.write_text(unit)
        command=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter',
                 '-I',ROOT,cpp,'-o',binary]
        if args.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
        subprocess.run(list(map(str,command)),check=True);subprocess.run([str(binary)],check=True)
